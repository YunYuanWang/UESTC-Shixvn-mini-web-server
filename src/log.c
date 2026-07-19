/*
 * log.c — Logging system v1.2
 *
 * Features:
 *   - Dual-file output: system log + access log
 *   - Four log levels: DEBUG, INFO, WARNING, ERROR
 *   - Line-count-based log rotation with configurable thresholds
 *   - Simple-string API (backward compatible) + printf-style API
 *   - Access log in simplified combined format
 *   - Thread-safe via flockfile/funlockfile
 *   - Post-fork reopen support
 */

#include "../include/log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- internal state ---- */
static FILE       *g_system_log   = NULL;
static FILE       *g_access_log   = NULL;
static log_level_t g_log_level    = LOG_INFO;   /* default: INFO and above */
static int         g_max_lines    = 0;
static int         g_max_roll     = 0;
static int         g_system_lines = 0;
static int         g_access_lines = 0;

/* saved paths for log_reopen() post-fork */
static char g_system_path[256];
static char g_access_path[256];

/* ---- forward declarations ---- */
static void log_write_system(log_level_t level, const char *msg);
static void rotate_log(const char *path, FILE **fp, int *line_count);

/* ---- level-to-label helper ---- */
static const char *level_label(log_level_t level) {
    switch (level) {
        case LOG_DEBUG:   return "DEBUG";
        case LOG_INFO:    return "INFO ";
        case LOG_WARNING: return "WARN ";
        case LOG_ERROR:   return "ERROR";
        default:          return "?????";
    }
}

/* ================================================================
 *  Initialization
 * ================================================================ */

int log_init(const char *system_log_path, const char *access_log_path,
             int max_lines, int max_roll_files) {

    g_max_lines = max_lines;
    g_max_roll  = max_roll_files;
    g_system_lines = 0;
    g_access_lines = 0;

    /* ---- system log ---- */
    if (system_log_path != NULL && system_log_path[0] != '\0') {
        g_system_log = fopen(system_log_path, "a");
        if (g_system_log == NULL) {
            fprintf(stderr, "[LOG] ERROR: cannot open system log: %s\n",
                    system_log_path);
            return -1;
        }
        strncpy(g_system_path, system_log_path, sizeof(g_system_path) - 1);
        g_system_path[sizeof(g_system_path) - 1] = '\0';

        /* count existing lines */
        {
            FILE *tmp = fopen(system_log_path, "r");
            if (tmp != NULL) {
                int ch;
                while ((ch = fgetc(tmp)) != EOF) {
                    if (ch == '\n') g_system_lines++;
                }
                fclose(tmp);
            }
        }
    } else {
        g_system_log = NULL;
        g_system_path[0] = '\0';
    }

    /* ---- access log ---- */
    if (access_log_path != NULL && access_log_path[0] != '\0') {
        /* if same path as system log, share the FILE* */
        if (system_log_path != NULL &&
            strcmp(system_log_path, access_log_path) == 0) {
            g_access_log = g_system_log;
            g_access_lines = g_system_lines;
            g_access_path[0] = '\0';
        } else {
            g_access_log = fopen(access_log_path, "a");
            if (g_access_log == NULL) {
                fprintf(stderr, "[LOG] ERROR: cannot open access log: %s\n",
                        access_log_path);
                /* non-fatal: system log still works */
            }
            strncpy(g_access_path, access_log_path, sizeof(g_access_path) - 1);
            g_access_path[sizeof(g_access_path) - 1] = '\0';

            /* count existing lines */
            if (g_access_log != NULL) {
                FILE *tmp = fopen(access_log_path, "r");
                if (tmp != NULL) {
                    int ch;
                    while ((ch = fgetc(tmp)) != EOF) {
                        if (ch == '\n') g_access_lines++;
                    }
                    fclose(tmp);
                }
            }
        }
    } else {
        g_access_log = NULL;
        g_access_path[0] = '\0';
    }

    /* ---- initial log entry (use log_write_system directly to avoid
     *      infinite recursion since log_infof would call back into us) ---- */
    log_write_system(LOG_INFO, "Logging system initialized (v1.2)");

    return 0;
}

int log_init_single(const char *path) {
    return log_init(path, NULL, 0, 0);
}

/*
 * log_reopen — close inherited FILE* handles and reopen from saved paths.
 * Each worker must call this after fork() to avoid buffer corruption.
 */
int log_reopen(void) {
    int ret = 0;

    /* close and reopen system log */
    if (g_system_path[0] != '\0' && g_system_log != NULL) {
        /* don't close if access log shares the same FILE* */
        if (g_access_log == g_system_log) {
            g_access_log = NULL;
        }
        fclose(g_system_log);
        g_system_log = fopen(g_system_path, "a");
        if (g_system_log == NULL) {
            ret = -1;
        } else {
            g_system_lines = 0;
        }
    }

    /* close and reopen access log */
    if (g_access_path[0] != '\0' && g_access_log != NULL) {
        fclose(g_access_log);
        g_access_log = fopen(g_access_path, "a");
        if (g_access_log == NULL) {
            ret = -1;
        } else {
            g_access_lines = 0;
        }
    } else if (g_access_path[0] == '\0' && g_system_path[0] != '\0') {
        /* access log shares system log — restore the pointer */
        g_access_log = g_system_log;
    }

    return ret;
}

/* ================================================================
 *  Log level control
 * ================================================================ */

void log_set_level(log_level_t level) {
    g_log_level = level;
}

log_level_t log_get_level(void) {
    return g_log_level;
}

/* ================================================================
 *  Log rotation
 * ================================================================ */

static void rotate_log(const char *path, FILE **fp, int *line_count) {
    int i;
    char old_name[320];
    char new_name[320];

    if (path == NULL || path[0] == '\0' || fp == NULL || *fp == NULL) {
        return;
    }

    /* close current file */
    fclose(*fp);
    *fp = NULL;

    /* shift old log files: remove oldest, rename .N-1 → .N */
    for (i = g_max_roll; i >= 1; i--) {
        if (i == g_max_roll) {
            /* remove oldest */
            snprintf(old_name, sizeof(old_name), "%s.%d", path, i);
            remove(old_name);
        } else {
            snprintf(old_name, sizeof(old_name), "%s.%d", path, i);
            snprintf(new_name, sizeof(new_name), "%s.%d", path, i + 1);
            rename(old_name, new_name);
        }
    }

    /* rename current .log → .log.1 */
    if (g_max_roll >= 1) {
        snprintf(new_name, sizeof(new_name), "%s.1", path);
        rename(path, new_name);
    }

    /* open new log file */
    *fp = fopen(path, "a");
    *line_count = 0;

    if (*fp == NULL) {
        fprintf(stderr, "[LOG] ERROR: failed to reopen log after rotation: %s\n",
                path);
    }
}

/* ================================================================
 *  Core write function — writes a single line to the system log
 * ================================================================ */

static void log_write_system(log_level_t level, const char *msg) {
    struct timeval tv;
    struct tm tm_info;
    char time_buf[40];

    if (g_system_log == NULL || msg == NULL) {
        return;
    }

    /* check rotation threshold before writing */
    if (g_max_lines > 0 && g_system_lines >= g_max_lines &&
        g_system_path[0] != '\0') {
        rotate_log(g_system_path, &g_system_log, &g_system_lines);
        if (g_system_log == NULL) return;
    }

    /* build timestamp */
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    /* thread-safe write */
    flockfile(g_system_log);
    fprintf(g_system_log, "[%s.%06ld] [%s] [PID %d] [TID %lu] %s\n",
            time_buf, (long)tv.tv_usec,
            level_label(level),
            (int)getpid(),
            (unsigned long)pthread_self(),
            msg);
    funlockfile(g_system_log);
    fflush(g_system_log);

    g_system_lines++;
}

/*
 * Core write function for printf-style messages.
 */
static void log_write_system_vfmt(log_level_t level, const char *fmt, va_list ap) {
    char msg_buf[2048];

    if (g_system_log == NULL || fmt == NULL) return;

    vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
    log_write_system(level, msg_buf);
}

/* ================================================================
 *  System log — simple string API (backward compatible)
 * ================================================================ */

void log_debug(const char *message) {
    if (g_log_level > LOG_DEBUG || message == NULL) return;
    log_write_system(LOG_DEBUG, message);
}

void log_info(const char *message) {
    if (g_log_level > LOG_INFO || message == NULL) return;
    log_write_system(LOG_INFO, message);
}

void log_warning(const char *message) {
    if (g_log_level > LOG_WARNING || message == NULL) return;
    log_write_system(LOG_WARNING, message);
}

void log_error(const char *message) {
    if (g_log_level > LOG_ERROR || message == NULL) return;
    log_write_system(LOG_ERROR, message);
}

/* ================================================================
 *  System log — printf-style API
 * ================================================================ */

void log_debugf(const char *fmt, ...) {
    if (g_log_level > LOG_DEBUG || fmt == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    log_write_system_vfmt(LOG_DEBUG, fmt, ap);
    va_end(ap);
}

void log_infof(const char *fmt, ...) {
    if (g_log_level > LOG_INFO || fmt == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    log_write_system_vfmt(LOG_INFO, fmt, ap);
    va_end(ap);
}

void log_warningf(const char *fmt, ...) {
    if (g_log_level > LOG_WARNING || fmt == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    log_write_system_vfmt(LOG_WARNING, fmt, ap);
    va_end(ap);
}

void log_errorf(const char *fmt, ...) {
    if (g_log_level > LOG_ERROR || fmt == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    log_write_system_vfmt(LOG_ERROR, fmt, ap);
    va_end(ap);
}

/* ================================================================
 *  System log — explicit PID variants (backward compatible)
 * ================================================================ */

void log_info_pid(pid_t pid, const char *message) {
    char buf[2048];
    if (g_log_level > LOG_INFO || message == NULL) return;
    snprintf(buf, sizeof(buf), "[PID %d] %s", (int)pid, message);
    log_write_system(LOG_INFO, buf);
}

void log_error_pid(pid_t pid, const char *message) {
    char buf[2048];
    if (g_log_level > LOG_ERROR || message == NULL) return;
    snprintf(buf, sizeof(buf), "[PID %d] %s", (int)pid, message);
    log_write_system(LOG_ERROR, buf);
}

/* ================================================================
 *  Access log
 * ================================================================ */

void log_access(const char *client_ip, const char *method,
                const char *path, int status, int bytes_sent) {
    struct timeval tv;
    struct tm tm_info;
    char time_buf[40];
    FILE *fp;

    fp = (g_access_log != NULL) ? g_access_log : g_system_log;
    if (fp == NULL) return;
    if (client_ip == NULL) client_ip = "-";
    if (method == NULL) method = "-";
    if (path == NULL) path = "-";

    /* check rotation threshold */
    if (g_max_lines > 0 && g_access_lines >= g_max_lines) {
        const char *rot_path = (g_access_path[0] != '\0')
                               ? g_access_path : g_system_path;
        if (rot_path[0] != '\0') {
            rotate_log(rot_path, &fp, &g_access_lines);
            if (fp == NULL) return;
            /* update the correct pointer */
            if (g_access_path[0] != '\0') {
                g_access_log = fp;
            } else {
                g_system_log = fp;
                g_access_log = fp;
            }
        }
    }

    /* build timestamp in simplified combined log format */
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%d/%b/%Y:%H:%M:%S %z", &tm_info);

    /* thread-safe write */
    flockfile(fp);
    fprintf(fp, "[%s] %s %s %s %d %d\n",
            time_buf, client_ip, method, path, status, bytes_sent);
    funlockfile(fp);
    fflush(fp);

    g_access_lines++;
}

/* ================================================================
 *  Cleanup
 * ================================================================ */

void log_close(void) {
    log_write_system(LOG_INFO, "Logging system shutting down");

    if (g_system_log != NULL) {
        fclose(g_system_log);
        g_system_log = NULL;
    }

    /*
     * If access log is separate from system log, close it too.
     * If they share the same FILE*, we already closed it above.
     */
    if (g_access_log != NULL && g_access_log != g_system_log) {
        fclose(g_access_log);
    }
    g_access_log = NULL;
}
