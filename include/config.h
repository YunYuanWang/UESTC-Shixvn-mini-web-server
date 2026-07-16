#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char server_name[64];
    char host[64];
    int  port;
    char www_root[128];          /* v0.10: renamed from root */
    char user_file[128];         /* v0.10: renamed from data_path */
    char log_path[128];
    int  max_connections;        /* v0.10: max concurrent connections */
    int  max_request_bytes;      /* v0.10: max request body size in bytes */

    /* legacy field aliases (maintained for backward compatibility) */
    char root[128];              /* deprecated — use www_root */
    char data_path[128];         /* deprecated — use user_file */
} server_config_t;

int load_config(const char *path, server_config_t *config);
void print_config(const server_config_t *config);

#endif
