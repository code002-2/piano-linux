#define _GNU_SOURCE
#include "log.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-journal.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>

/* Internal state */
static LogLevel g_level       = UPERF_LOG_INFO;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE  *g_log_fp        = NULL;
static int    g_use_journald  = 0;

/* Format a timestamp string like "2026-07-10 14:32:01.234" */
static void format_timestamp(char *buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
    char tmp[8];
    unsigned int millis = (unsigned int)(ts.tv_nsec / 1000000L);
    snprintf(tmp, sizeof(tmp), ".%03u", millis);
    strncat(buf, tmp, len - strlen(buf) - 1);
}

/* Map the project-specific levels to syslog priorities. */
static int syslog_priority(LogLevel level) {
    switch (level) {
        case UPERF_LOG_DEBUG:  return LOG_DEBUG;
        case UPERF_LOG_INFO:   return LOG_INFO;
        case UPERF_LOG_WARN:   return LOG_WARNING;
        case UPERF_LOG_ERROR:  return LOG_ERR;
        case UPERF_LOG_FATAL:  return LOG_CRIT;
        default:         return 6;
    }
}

void log_impl(LogLevel level, const char *file, int line, const char *func,
              const char *fmt, ...) {
    if (level < UPERF_LOG_DEBUG || level >= UPERF_LOG_NUM_LEVELS)
        return;

    if (level < g_level)
        return;

    pthread_mutex_lock(&g_mutex);

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    /* Build formatted message */
    va_list ap;
    va_start(ap, fmt);
    char msg_buf[1024];
    int len = vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
    va_end(ap);
    if (len < 0) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    msg_buf[sizeof(msg_buf) - 1] = '\0';

    /* Prefix: [timestamp] [LEVEL] file:line func: message */
    const char *level_str[] = {"DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    char full_msg[2048];
    snprintf(full_msg, sizeof(full_msg), "[%s] [%s] %s:%d %s: %s",
             ts, level_str[level], file, line, func, msg_buf);

    /* Write to file/stderr */
    if (g_log_fp) {
        fputs(full_msg, g_log_fp);
        fputc('\n', g_log_fp);
        fflush(g_log_fp);
    } else {
        fprintf(stderr, "%s\n", full_msg);
    }

    /* Write to journald if available */
#ifdef HAVE_LIBSYSTEMD
    if (g_use_journald) {
        int pri = syslog_priority(level);
        sd_journal_print(pri, "%s", full_msg);
    }
#endif

    /* Write to syslog as fallback if no journald */
#ifndef HAVE_LIBSYSTEMD
    if (!g_use_journald) {
        syslog(syslog_priority(level), "%s", msg_buf);
    }
#endif

    /* FATAL: flush and return (caller should call exit()/abort()) */
    if (level == UPERF_LOG_FATAL) {
        if (g_log_fp)
            fflush(g_log_fp);
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    pthread_mutex_unlock(&g_mutex);
}

int log_init(LogLevel level, int use_journald, const char *log_file) {
    if (level < UPERF_LOG_DEBUG || level >= UPERF_LOG_NUM_LEVELS)
        level = UPERF_LOG_INFO;

    g_level = level;
    g_use_journald = use_journald;

    if (log_file) {
        g_log_fp = fopen(log_file, "a");
        if (!g_log_fp) {
            fprintf(stderr, "[uperf-linux] Failed to open log file '%s': %s\n",
                    log_file, strerror(errno));
            g_log_fp = NULL;
        }
    }

    log_info("Logging initialized: level=%d journald=%d file=%s",
             level, use_journald, log_file ? log_file : "(stderr)");
    return 0;
}

void log_set_level(LogLevel level) {
    pthread_mutex_lock(&g_mutex);
    g_level = level;
    pthread_mutex_unlock(&g_mutex);
    log_info("Log level changed to %d", level);
}

LogLevel log_get_level(void) {
    pthread_mutex_lock(&g_mutex);
    LogLevel lvl = g_level;
    pthread_mutex_unlock(&g_mutex);
    return lvl;
}

void log_shutdown(void) {
    log_debug("Logging shutdown");
    pthread_mutex_lock(&g_mutex);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    pthread_mutex_unlock(&g_mutex);
    pthread_mutex_destroy(&g_mutex);
}
