#include "../include/log.h"
#include <pthread.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static FILE *log_file = NULL;

int log_init(const char *path) {
    if (path == NULL) {
        return -1;
    }

    log_file = fopen(path, "a");
    if (log_file == NULL) {
        return -1;
    }

    return 0;
}

/*
 * log_write — 所有日志输出的统一入口。
 * 每条日志必定包含 PID，调用者无需关心 pid 参数。
 * 调用 log_info/log_error 时自动使用 getpid()；
 * 调用 log_info_pid/log_error_pid 时使用显式传入的 pid
 * （用于父进程记录子进程 PID 等场景）。
 */
static void log_write(const char *level, pid_t pid, const char *message) {
    if (log_file == NULL || message == NULL) {
        return;
    }

    struct timeval tv;
    struct tm tm_info;
    char time_buf[32];

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(log_file, "[%s] [PID %d] [TID %lu] [%s.%06ld] %s\n",
            level, (int)pid, (unsigned long)pthread_self(),
            time_buf, tv.tv_usec, message);
    fflush(log_file);
}

void log_info(const char *message) {
    log_write("INFO", getpid(), message);
}

void log_error(const char *message) {
    log_write("ERROR", getpid(), message);
}

void log_info_pid(pid_t pid, const char *message) {
    log_write("INFO", pid, message);
}

void log_error_pid(pid_t pid, const char *message) {
    log_write("ERROR", pid, message);
}

void log_close(void) {
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
}
