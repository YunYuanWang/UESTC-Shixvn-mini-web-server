#ifndef LOG_H
#define LOG_H

#include <sys/types.h>

int log_init(const char *path);
int log_reopen(const char *path);   /* v1.0: close inherited FILE* and reopen (post-fork) */
void log_info(const char *message);
void log_error(const char *message);
void log_info_pid(pid_t pid, const char *message);
void log_error_pid(pid_t pid, const char *message);
void log_close(void);

#endif
