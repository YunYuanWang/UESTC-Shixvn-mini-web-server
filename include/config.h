#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char server_name[64];
    char host[64];
    int  port;
    char www_root[128];          /* v0.10: renamed from root */
    char user_file[128];         /* v0.10: renamed from data_path */
    char log_path[128];
    char system_log[128];          /* v1.2: separate system log path */
    char access_log[128];          /* v1.2: separate access log path */
    int  log_max_lines;            /* v1.2: rotate after N lines (0=no rotation) */
    int  log_max_roll_files;       /* v1.2: keep N old log files */
    int  max_connections;          /* v0.10: max concurrent connections */
    int  max_request_bytes;      /* v0.10: max request body size in bytes */

    /* v1.0: master-worker process model */
    int  worker_processes;            /* number of worker processes (default: 2) */
    int  worker_shutdown_timeout_ms;  /* grace period for worker shutdown (default: 3000) */

    /* legacy field aliases (maintained for backward compatibility) */
    char root[128];              /* deprecated — use www_root */
    char data_path[128];         /* deprecated — use user_file */
} server_config_t;

int load_config(const char *path, server_config_t *config);
void print_config(const server_config_t *config);

#endif
