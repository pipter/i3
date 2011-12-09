/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 * © 2009-2011 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * log.c: Setting of loglevels, logging functions.
 *
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#include "util.h"
#include "log.h"
#include "i3.h"
#include "libi3.h"

/* loglevels.h is autogenerated at make time */
#include "loglevels.h"

static uint64_t loglevel = 0;
static bool verbose = false;
static FILE *errorfile;
char *errorfilename;

/* SHM logging variables */

/* The name for the SHM (/i3-log-%pid). Will end up on /dev/shm on most
 * systems. Global so that we can clean up at exit. */
char *shmlogname = "";
/* Size limit for the SHM log, by default 25 MiB. Can be overwritten using the
 * flag --shmlog-size. */
int shmlog_size = 25 * 1024 * 1024;
/* If enabled, logbuffer will point to a memory mapping of the i3 SHM log. */
static char *logbuffer;
/* A pointer (within logbuffer) where data will be written to next. */
static char *logwalk;
/* A pointer to the byte where we last wrapped. Necessary to not print the
 * left-overs at the end of the ringbuffer. */
static char *loglastwrap;
/* Size (in bytes) of the i3 SHM log. */
static int logbuffer_size;
/* File descriptor for shm_open. */
static int logbuffer_shm;

/*
 * Initializes logging by creating an error logfile in /tmp (or
 * XDG_RUNTIME_DIR, see get_process_filename()).
 *
 * Will be called twice if --shmlog-size is specified.
 *
 */
void init_logging() {
    if (!errorfilename) {
        if (!(errorfilename = get_process_filename("errorlog")))
            ELOG("Could not initialize errorlog\n");
        else {
            errorfile = fopen(errorfilename, "w");
            if (fcntl(fileno(errorfile), F_SETFD, FD_CLOEXEC)) {
                ELOG("Could not set close-on-exec flag\n");
            }
        }
    }

    /* If this is a debug build (not a release version), we will enable SHM
     * logging by default, unless the user turned it off explicitly. */
    if (logbuffer == NULL && shmlog_size > 0) {
        /* Reserve 1% of the RAM for the logfile, but at max 25 MiB.
         * For 512 MiB of RAM this will lead to a 5 MiB log buffer.
         * At the moment (2011-12-10), no testcase leads to an i3 log
         * of more than ~ 600 KiB. */
        long long physical_mem_bytes = (long long)sysconf(_SC_PHYS_PAGES) *
                                                  sysconf(_SC_PAGESIZE);
        logbuffer_size = min(physical_mem_bytes * 0.01, shmlog_size);
        sasprintf(&shmlogname, "/i3-log-%d", getpid());
        logbuffer_shm = shm_open(shmlogname, O_RDWR | O_CREAT | O_TRUNC, S_IREAD | S_IWRITE);
        if (logbuffer_shm == -1) {
            ELOG("Could not shm_open SHM segment for the i3 log: %s\n", strerror(errno));
            return;
        }

        if (ftruncate(logbuffer_shm, logbuffer_size) == -1) {
            close(logbuffer_shm);
            shm_unlink("/i3-log-");
            ELOG("Could not ftruncate SHM segment for the i3 log: %s\n", strerror(errno));
            return;
        }

        logbuffer = mmap(NULL, logbuffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, logbuffer_shm, 0);
        if (logbuffer == MAP_FAILED) {
            close(logbuffer_shm);
            shm_unlink("/i3-log-");
            ELOG("Could not mmap SHM segment for the i3 log: %s\n", strerror(errno));
            logbuffer = NULL;
            return;
        }
        logwalk = logbuffer;
        loglastwrap = logbuffer + logbuffer_size;
    }
}

/*
 * Set verbosity of i3. If verbose is set to true, informative messages will
 * be printed to stdout. If verbose is set to false, only errors will be
 * printed.
 *
 */
void set_verbosity(bool _verbose) {
    verbose = _verbose;
}

/*
 * Enables the given loglevel.
 *
 */
void add_loglevel(const char *level) {
    /* Handle the special loglevel "all" */
    if (strcasecmp(level, "all") == 0) {
        loglevel = UINT64_MAX;
        return;
    }

    for (int i = 0; i < sizeof(loglevels) / sizeof(char*); i++) {
        if (strcasecmp(loglevels[i], level) != 0)
            continue;

        /* The position in the array (plus one) is the amount of times
         * which we need to shift 1 to the left to get our bitmask for
         * the specific loglevel. */
        loglevel |= (1 << (i+1));
        break;
    }
}

/*
 * Returns the offsets for the next write and for the last wrap.
 * Necessary to print the i3 SHM log in the correct order.
 *
 */
void get_log_markers(int *offset_next_write, int *offset_last_wrap, int *size) {
    *offset_next_write = (logwalk - logbuffer);
    *offset_last_wrap = (loglastwrap - logbuffer);
    *size = logbuffer_size;
}

/*
 * Logs the given message to stdout (if print is true) while prefixing the
 * current time to it. Additionally, the message will be saved in the i3 SHM
 * log if enabled.
 * This is to be called by *LOG() which includes filename/linenumber/function.
 *
 */
static void vlog(const bool print, const char *fmt, va_list args) {
    /* Precisely one page to not consume too much memory but to hold enough
     * data to be useful. */
    static char message[4096];
    static struct tm result;
    static time_t t;
    static struct tm *tmp;
    static size_t len;

    /* Get current time */
    t = time(NULL);
    /* Convert time to local time (determined by the locale) */
    tmp = localtime_r(&t, &result);
    /* Generate time prefix */
    len = strftime(message, sizeof(message), "%x %X - ", tmp);

    /*
     * logbuffer  print
     * ----------------
     *  true      true   format message, save, print
     *  true      false  format message, save
     *  false     true   print message only
     *  false     false  INVALID, never called
     */
    if (!logbuffer) {
#ifdef DEBUG_TIMING
        struct timeval tv;
        gettimeofday(&tv, NULL);
        printf("%s%d.%d - ", message, tv.tv_sec, tv.tv_usec);
#else
        printf("%s", message);
#endif
        vprintf(fmt, args);
    } else {
        len += vsnprintf(message + len, sizeof(message) - len, fmt, args);
        if (len == sizeof(message)) {
            fprintf(stderr, "BUG: single log message > 4k\n");
        }
        /* If there is no space for the current message (plus trailing
         * nullbyte) in the ringbuffer, we need to wrap and write to the
         * beginning again. */
        if ((len+1) >= (logbuffer_size - (logwalk - logbuffer))) {
            loglastwrap = logwalk;
            logwalk = logbuffer;
        }

        /* Copy the buffer, terminate it, move the write pointer to the byte after
         * our current message. */
        strncpy(logwalk, message, len);
        logwalk[len] = '\0';
        logwalk += len + 1;

        if (print)
            fwrite(message, len, 1, stdout);
    }
}

/*
 * Logs the given message to stdout while prefixing the current time to it,
 * but only if verbose mode is activated.
 *
 */
void verboselog(char *fmt, ...) {
    va_list args;

    if (!logbuffer && !verbose)
        return;

    va_start(args, fmt);
    vlog(verbose, fmt, args);
    va_end(args);
}

/*
 * Logs the given message to stdout while prefixing the current time to it.
 *
 */
void errorlog(char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vlog(true, fmt, args);
    va_end(args);

    /* also log to the error logfile, if opened */
    va_start(args, fmt);
    vfprintf(errorfile, fmt, args);
    fflush(errorfile);
    va_end(args);
}

/*
 * Logs the given message to stdout while prefixing the current time to it,
 * but only if the corresponding debug loglevel was activated.
 * This is to be called by DLOG() which includes filename/linenumber
 *
 */
void debuglog(uint64_t lev, char *fmt, ...) {
    va_list args;

    if (!logbuffer && !(loglevel & lev))
        return;

    va_start(args, fmt);
    vlog((loglevel & lev), fmt, args);
    va_end(args);
}
