#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char server_name[64];
    char host[64];
    int port;
    char root[128];
    char data_path[128];
    char log_path[128];
} server_config_t;

int load_config(const char *path, server_config_t *config);
void print_config(const server_config_t *config);

#endif
