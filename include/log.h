#ifndef LOG_H
#define LOG_H

#include <sys/types.h>

/* ================================================================
 * Logging System v1.2
 *
 * Features:
 *   - Separate system log and access log files
 *   - Four log levels: DEBUG, INFO, WARNING, ERROR
 *   - Log rotation by line count (configurable threshold)
 *   - printf-style format variants for system log (log_*f)
 *   - Structured access log (simplified combined format)
 *   - Thread-safe writes (flockfile/funlockfile)
 *   - Post-fork log_reopen() for worker processes
 *
 * Backward compatibility:
 *   log_info(const char *msg) and log_error(const char *msg) retain
 *   their original single-string signature — all existing call sites
 *   remain unchanged.  New code should prefer log_infof() / log_errorf()
 *   for printf-style formatting.
 * ================================================================ */

/* ---- log levels ---- */
typedef enum {
    LOG_DEBUG   = 0,
    LOG_INFO    = 1,
    LOG_WARNING = 2,
    LOG_ERROR   = 3,
    LOG_NONE    = 4    /* disable all logging */
} log_level_t;

/* ---- initialization ---- */

/*
 * Initialize logging with separate system and access log files.
 * If system_log_path or access_log_path is NULL, that log is disabled.
 * max_lines: rotate after this many lines (0 = no rotation).
 * max_roll_files: keep at most this many old log files (0 = delete old).
 * Returns 0 on success, -1 on error.
 */
int log_init(const char *system_log_path, const char *access_log_path,
             int max_lines, int max_roll_files);

/*
 * Convenience: initialize with a single log file (backward compatible).
 * Equivalent to log_init(path, NULL, 0, 0).
 */
int log_init_single(const char *path);

/*
 * Reopen log files after fork() — each worker process must call this
 * to get independent FILE* handles and avoid cross-process buffer
 * corruption.  Uses internally saved paths from log_init().
 * Returns 0 on success, -1 on error.
 */
int log_reopen(void);

/* ---- log level control ---- */

/* Set the minimum log level.  Messages below this level are dropped. */
void log_set_level(log_level_t level);

/* Get the current log level. */
log_level_t log_get_level(void);

/* ---- system log — simple string API (backward compatible) ---- */

void log_info(const char *message);          /* LOG_INFO level */
void log_error(const char *message);         /* LOG_ERROR level */
void log_warning(const char *message);       /* LOG_WARNING level (new) */
void log_debug(const char *message);         /* LOG_DEBUG level (new) */

/* ---- system log — printf-style API (use in new code) ---- */

void log_infof(const char *fmt, ...)    __attribute__((format(printf, 1, 2)));
void log_errorf(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));
void log_warningf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_debugf(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));

/* ---- system log — explicit PID variants (backward compatible) ---- */

void log_info_pid(pid_t pid, const char *message);
void log_error_pid(pid_t pid, const char *message);

/* ---- access log (structured) ---- */

/*
 * Log an HTTP access record in simplified combined log format:
 *   [dd/Mon/yyyy:HH:MM:SS tz] IP METHOD PATH STATUS BYTES
 */
void log_access(const char *client_ip, const char *method,
                const char *path, int status, int bytes_sent);

/* ---- cleanup ---- */

void log_close(void);

#endif /* LOG_H */
