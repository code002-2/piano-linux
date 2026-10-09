#ifndef UPERF_LOG_H
#define UPERF_LOG_H

#include <stddef.h>

/* Log severity levels */
typedef enum {
    UPERF_LOG_DEBUG = 0,
    UPERF_LOG_INFO  = 1,
    UPERF_LOG_WARN  = 2,
    UPERF_LOG_ERROR = 3,
    UPERF_LOG_FATAL = 4,
    UPERF_LOG_NUM_LEVELS
} LogLevel;

/* Initialize the logging subsystem.
 * level: minimum severity to output (0-4)
 * use_journald: nonzero to also log via systemd-journald
 * log_file: if non-NULL, append to this file (NULL = stderr only)
 * Returns 0 on success, -1 on failure. */
int log_init(LogLevel level, int use_journald, const char *log_file);

/* Reset logging level at runtime (called on SIGHUP). */
void log_set_level(LogLevel level);

/* Get current log level. */
LogLevel log_get_level(void);

/* Core logging function — use the macro versions below in practice. */
void log_impl(LogLevel level, const char *file, int line, const char *func,
              const char *fmt, ...);

/* Convenience macros that inject file/line/function. */
#define log_debug(...)  log_impl(UPERF_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_info(...)   log_impl(UPERF_LOG_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_warn(...)   log_impl(UPERF_LOG_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_error(...)  log_impl(UPERF_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define log_fatal(...)  log_impl(UPERF_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* Shutdown logging subsystem and free resources. */
void log_shutdown(void);

#endif /* UPERF_LOG_H */
