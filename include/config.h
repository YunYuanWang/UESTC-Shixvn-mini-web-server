#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — Server configuration with Nginx-style virtual host support (v1.5)
 *
 * Supports two config formats:
 *   1. Legacy flat key=value (backward compatible)
 *   2. Nginx-style server { } blocks with server_name for name-based virtual hosting
 *
 * v1.5 adds:
 *   - log_level / error_log directives
 *   - location { } blocks for config-based routing (via route_table_t)
 */

#include "log.h"          /* log_level_t */

/* ---- forward declarations ---- */
typedef struct route_table_s route_table_t;  /* defined in route_table.h */

/* ---- virtual host limits ---- */
#define MAX_SERVER_BLOCKS    16
#define MAX_SERVER_NAMES      8
#define MAX_SERVER_NAME_LEN  128

/* ---- per-site virtual host configuration ---- */
typedef struct {
    char host[64];                                    /* bind address (from listen) */
    int  port;                                        /* bind port (from listen) */
    char root[128];                                   /* document root directory */
    char access_log[128];                             /* per-site access log (empty = use global) */
    char server_names[MAX_SERVER_NAMES][MAX_SERVER_NAME_LEN];
    int  name_count;                                  /* number of server_name entries */
    int  is_default;                                  /* 1 = default_server flag */
} server_block_t;

/* ---- global + per-site server configuration ---- */
typedef struct {
    /* ---- global settings ---- */
    char server_name[64];       /* legacy: first block's server_name */
    char host[64];              /* legacy: first block's listen address */
    int  port;                  /* legacy: first block's listen port */
    char www_root[128];         /* legacy: first block's root (v0.10: renamed from root) */
    char user_file[128];        /* user database CSV path (v0.10: renamed from data_path) */
    char log_path[128];         /* global fallback log path */
    char system_log[128];       /* system log path (v1.2) */
    char access_log[128];       /* global access log path (v1.2) */
    char auth_user_file[128];    /* .htpasswd file path (v1.6) */
    log_level_t log_level;       /* minimum log level (v1.5) */
    char error_log[128];         /* error log path (v1.5) */
    log_level_t error_log_level; /* error log filter level (v1.5) */
    int  log_max_lines;          /* rotate after N lines (0=no rotation, v1.2) */
    int  log_max_roll_files;     /* keep N old log files (v1.2) */
    int  max_connections;       /* max concurrent connections (v0.10) */
    int  max_request_bytes;     /* max request body size in bytes (v0.10) */

    /* v1.0: master-worker process model */
    int  worker_processes;            /* number of worker processes (default: 2) */
    int  worker_shutdown_timeout_ms;  /* grace period for worker shutdown (default: 3000) */

    /* ---- v1.2.1: virtual host server blocks ---- */
    server_block_t servers[MAX_SERVER_BLOCKS];
    int  server_count;

    /* ---- v1.5: config-based route table ---- */
    route_table_t *route_table;          /* populated from location { } blocks */

    /* ---- legacy field aliases (maintained for backward compatibility) ---- */
    char root[128];             /* deprecated — use www_root */
    char data_path[128];        /* deprecated — use user_file */
} server_config_t;

/* ---- config file I/O ---- */
int load_config(const char *path, server_config_t *config);
void print_config(const server_config_t *config);

/*
 * Store the global server config for access by other modules that cannot
 * receive it as a parameter (e.g. request_handler).
 */
void config_set_global(const server_config_t *cfg);

/*
 * Retrieve the stored global server config, or NULL if not set.
 */
const server_config_t *config_get_global(void);

/*
 * Find the server block matching a given Host header value.
 * If config is NULL, uses the globally-stored config (from config_set_global).
 * Returns pointer to the matched server_block_t, or NULL if no config available.
 *
 * Matching logic:
 *   1. Strip port suffix from host_header (e.g. "example.com:8080" → "example.com")
 *   2. Case-insensitive exact match against each block's server_names[]
 *   3. Return first block with is_default==1 if no match
 *   4. Return servers[0] if no default_server is set
 *   5. Return NULL if server_count==0
 */
const server_block_t *config_find_server(const server_config_t *config,
                                          const char *host_header);

/*
 * v1.5: Retrieve the global route table (from the globally-stored config).
 * Returns NULL if no config has been set.
 */
route_table_t *config_get_route_table(void);

#endif
