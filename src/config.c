/*
 * config.c — Server configuration parser v1.2.1
 *
 * Supports two formats:
 *   1. Legacy flat key=value  (auto-detected, backward compatible)
 *   2. Nginx-style server { } blocks with name-based virtual hosting
 */

#include "../include/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- utility helpers ---- */

static void remove_newline(char *text) {
    text[strcspn(text, "\r\n")] = '\0';
}

static char *trim(char *text) {
    char *end;

    while (isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}

/* Strip trailing semicolon from a value */
static char *strip_semicolon(char *text) {
    char *end;
    if (!text || *text == '\0') return text;
    end = text + strlen(text) - 1;
    if (*end == ';') *end = '\0';
    return text;
}

static void copy_text(char *dest, int dest_size, const char *src) {
    if (dest_size <= 0) {
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/* Case-insensitive string comparison */
static int strcasecmp_safe(const char *a, const char *b) {
    if (!a || !b) return -1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ---- global config storage (for modules without direct access) ---- */

static const server_config_t *g_global_config = NULL;

void config_set_global(const server_config_t *cfg) {
    g_global_config = cfg;
}

const server_config_t *config_get_global(void) {
    return g_global_config;
}

/* ---- config_find_server: Host-header → server_block matching ---- */

const server_block_t *config_find_server(const server_config_t *config,
                                          const char *host_header) {
    char host_clean[MAX_SERVER_NAME_LEN];
    const char *colon;
    int i, j;

    /* Use global config if NULL is passed */
    if (!config) config = g_global_config;
    if (!config || config->server_count <= 0) return NULL;
    if (!host_header || host_header[0] == '\0') {
        /* No Host header → find default_server */
        for (i = 0; i < config->server_count; i++) {
            if (config->servers[i].is_default) return &config->servers[i];
        }
        return &config->servers[0];
    }

    /* Strip port suffix (e.g. "example.com:8080" → "example.com") */
    copy_text(host_clean, sizeof(host_clean), host_header);
    colon = strchr(host_clean, ':');
    if (colon) *((char *)colon) = '\0';

    /* Case-insensitive exact match */
    for (i = 0; i < config->server_count; i++) {
        /* Empty name_count = catch-all (match anything) */
        if (config->servers[i].name_count == 0) {
            return &config->servers[i];
        }
        for (j = 0; j < config->servers[i].name_count; j++) {
            if (strcasecmp_safe(host_clean,
                                config->servers[i].server_names[j]) == 0) {
                return &config->servers[i];
            }
        }
    }

    /* No match → find default_server */
    for (i = 0; i < config->server_count; i++) {
        if (config->servers[i].is_default) return &config->servers[i];
    }

    /* Fallback to first block */
    return &config->servers[0];
}

/* ================================================================
 *  Legacy flat-format parser (unchanged logic)
 * ================================================================ */

static int parse_flat_line(const char *key, const char *value,
                           server_config_t *config) {
    if (strcmp(key, "server_name") == 0) {
        copy_text(config->server_name, sizeof(config->server_name), value);
    } else if (strcmp(key, "host") == 0) {
        copy_text(config->host, sizeof(config->host), value);
    } else if (strcmp(key, "port") == 0) {
        config->port = atoi(value);
    } else if (strcmp(key, "www_root") == 0) {
        copy_text(config->www_root, sizeof(config->www_root), value);
    } else if (strcmp(key, "root") == 0) {
        copy_text(config->root, sizeof(config->root), value);
    } else if (strcmp(key, "user_file") == 0) {
        copy_text(config->user_file, sizeof(config->user_file), value);
    } else if (strcmp(key, "data") == 0) {
        copy_text(config->data_path, sizeof(config->data_path), value);
    } else if (strcmp(key, "log") == 0) {
        copy_text(config->log_path, sizeof(config->log_path), value);
    } else if (strcmp(key, "system_log") == 0) {
        copy_text(config->system_log, sizeof(config->system_log), value);
    } else if (strcmp(key, "access_log") == 0) {
        copy_text(config->access_log, sizeof(config->access_log), value);
    } else if (strcmp(key, "log_max_lines") == 0) {
        config->log_max_lines = atoi(value);
    } else if (strcmp(key, "log_max_roll_files") == 0) {
        config->log_max_roll_files = atoi(value);
    } else if (strcmp(key, "max_connections") == 0) {
        config->max_connections = atoi(value);
    } else if (strcmp(key, "max_request_bytes") == 0) {
        config->max_request_bytes = atoi(value);
    } else if (strcmp(key, "worker_processes") == 0) {
        config->worker_processes = atoi(value);
    } else if (strcmp(key, "worker_shutdown_timeout_ms") == 0) {
        config->worker_shutdown_timeout_ms = atoi(value);
    }
    /* Unknown keys are silently ignored (same as before) */
    return 0;
}

static int load_legacy_config(FILE *fp, server_config_t *config) {
    char line[256];

    rewind(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key, *value, *separator;

        remove_newline(line);
        key = trim(line);

        if (key[0] == '\0' || key[0] == '#') continue;

        separator = strchr(key, '=');
        if (separator == NULL) continue;  /* skip lines without = */

        *separator = '\0';
        value = trim(separator + 1);
        key   = trim(key);

        if (key[0] == '\0' || value[0] == '\0') continue;

        parse_flat_line(key, value, config);
    }
    return 0;
}

/* ================================================================
 *  Nginx-style block parser (v1.2.1)
 * ================================================================ */

/* Parse "host:port [default_server]" from a listen directive */
static int parse_listen(const char *value, server_block_t *block) {
    char buf[128];
    char *colon, *space;
    char *rest;

    copy_text(buf, sizeof(buf), value);

    /* Check for default_server flag */
    rest = buf;
    block->is_default = 0;
    {
        /* Handle "default_server" appearing anywhere */
        char *ds = strstr(buf, "default_server");
        if (ds) {
            block->is_default = 1;
            /* Remove it from the string for IP:port parsing */
            memset(ds, ' ', strlen("default_server"));
        }
    }

    /* Trim again after removing default_server */
    rest = trim(buf);

    /* Split host:port */
    colon = strchr(rest, ':');
    if (colon) {
        *colon = '\0';
        /* Trim host part */
        {
            char *h = trim(rest);
            copy_text(block->host, sizeof(block->host), h);
        }
        /* Parse port */
        {
            char *p = trim(colon + 1);
            /* strip any trailing spaces/semicolons */
            space = strchr(p, ' ');
            if (space) *space = '\0';
            block->port = atoi(p);
        }
    } else {
        /* No colon — just a port? Or just an IP? Try as port. */
        block->port = atoi(rest);
        if (block->port <= 0) {
            /* Not a port — treat as host with default port 80 */
            copy_text(block->host, sizeof(block->host), rest);
            block->port = 80;
        }
    }

    if (block->port <= 0) block->port = 80;
    if (block->host[0] == '\0') copy_text(block->host, sizeof(block->host), "0.0.0.0");

    return 0;
}

/* Parse server_name "name1 name2 ..." */
static int parse_server_name(const char *value, server_block_t *block) {
    char buf[512];
    char *token;
    char *saveptr;

    copy_text(buf, sizeof(buf), value);

    block->name_count = 0;
    token = strtok_r(buf, " \t", &saveptr);
    while (token && block->name_count < MAX_SERVER_NAMES) {
        /* Skip empty tokens */
        if (token[0] != '\0') {
            copy_text(block->server_names[block->name_count],
                      MAX_SERVER_NAME_LEN, token);
            block->name_count++;
        }
        token = strtok_r(NULL, " \t", &saveptr);
    }
    return 0;
}

/* Parse a single directive inside a server { } block */
static int parse_block_directive(const char *key, const char *value,
                                  server_block_t *block) {
    if (strcmp(key, "listen") == 0) {
        return parse_listen(value, block);
    } else if (strcmp(key, "server_name") == 0) {
        return parse_server_name(value, block);
    } else if (strcmp(key, "root") == 0) {
        copy_text(block->root, sizeof(block->root), value);
    } else if (strcmp(key, "access_log") == 0) {
        copy_text(block->access_log, sizeof(block->access_log), value);
    }
    /* Unknown directives inside server block are ignored */
    return 0;
}

/* Parse one server { } block: read lines until "}" */
static int parse_server_block(FILE *fp, server_config_t *config) {
    server_block_t *block;
    char line[256];

    if (config->server_count >= MAX_SERVER_BLOCKS) {
        fprintf(stderr, "ERROR: too many server blocks (max %d)\n",
                MAX_SERVER_BLOCKS);
        return -1;
    }

    block = &config->servers[config->server_count];
    memset(block, 0, sizeof(*block));

    /* Defaults for a server block */
    block->port = 80;
    copy_text(block->host, sizeof(block->host), "0.0.0.0");

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *text, *separator;

        remove_newline(line);
        text = trim(line);

        if (text[0] == '\0' || text[0] == '#') continue;

        /* Closing brace → end of server block */
        if (text[0] == '}') {
            config->server_count++;
            return 0;
        }

        /* Strip trailing semicolon */
        text = strip_semicolon(text);

        /* Find separator — either '=' (key=value) or first space (key value) */
        separator = strchr(text, '=');
        if (separator) {
            *separator = '\0';
            {
                char *key   = trim(text);
                char *value = trim(separator + 1);
                if (key[0] != '\0') {
                    parse_block_directive(key, value, block);
                }
            }
        } else {
            /* Space-separated: "root www" or "listen 0.0.0.0:80" */
            separator = strchr(text, ' ');
            if (separator) {
                *separator = '\0';
                {
                    char *key   = trim(text);
                    char *value = trim(separator + 1);
                    if (key[0] != '\0') {
                        parse_block_directive(key, value, block);
                    }
                }
            }
        }
    }

    /* Reached EOF without closing brace */
    fprintf(stderr, "ERROR: server block not closed (missing '}')\n");
    return -1;
}

/* Parse Nginx-style config with top-level directives and server blocks */
static int load_nginx_config(FILE *fp, server_config_t *config) {
    char line[256];

    rewind(fp);
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *text, *separator;

        remove_newline(line);
        text = trim(line);

        if (text[0] == '\0' || text[0] == '#') continue;

        /* "server {" opens a new server block */
        if (strcmp(text, "server {") == 0 ||
            strncmp(text, "server{", 7) == 0) {
            if (parse_server_block(fp, config) != 0) {
                return -1;
            }
            continue;
        }

        /* Skip standalone "{" or "}" at top level */
        if (text[0] == '{' || text[0] == '}') continue;

        /* Strip trailing semicolon for top-level directives */
        text = strip_semicolon(text);

        /* Try "key=value" format first, then "key value" */
        separator = strchr(text, '=');
        if (separator) {
            *separator = '\0';
            {
                char *key   = trim(text);
                char *value = trim(separator + 1);
                if (key[0] != '\0') {
                    parse_flat_line(key, value, config);
                }
            }
        } else {
            /* Space-separated: "worker_processes 2" */
            separator = strchr(text, ' ');
            if (separator) {
                *separator = '\0';
                {
                    char *key   = trim(text);
                    char *value = trim(separator + 1);
                    if (key[0] != '\0') {
                        parse_flat_line(key, value, config);
                    }
                }
            }
        }
    }

    return 0;
}

/* ================================================================
 *  Detection: does the file contain Nginx-style server blocks?
 * ================================================================ */

static int has_server_blocks(const char *path) {
    FILE *fp;
    char line[256];
    int found = 0;

    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *text = trim(line);
        remove_newline(text);
        text = trim(text);
        if (strcmp(text, "server {") == 0 ||
            strncmp(text, "server{", 7) == 0) {
            found = 1;
            break;
        }
    }

    fclose(fp);
    return found;
}

/* ================================================================
 *  Post-parse: populate legacy fields, apply defaults, validate
 * ================================================================ */

static void config_post_process(server_config_t *config) {
    /* If we have server blocks, populate legacy fields from the first block */
    if (config->server_count > 0) {
        if (config->host[0] == '\0') {
            copy_text(config->host, sizeof(config->host),
                      config->servers[0].host);
        }
        if (config->port <= 0) {
            config->port = config->servers[0].port;
        }
        if (config->www_root[0] == '\0') {
            copy_text(config->www_root, sizeof(config->www_root),
                      config->servers[0].root);
        }
        if (config->server_name[0] == '\0' &&
            config->servers[0].name_count > 0) {
            copy_text(config->server_name, sizeof(config->server_name),
                      config->servers[0].server_names[0]);
        }
        /* Propagate legacy values back to server blocks if empty */
        {
            int i;
            for (i = 0; i < config->server_count; i++) {
                if (config->servers[i].port <= 0)
                    config->servers[i].port = config->port;
                if (config->servers[i].host[0] == '\0')
                    copy_text(config->servers[i].host,
                              sizeof(config->servers[i].host), config->host);
            }
        }
    }

    /* Backward compatibility: cross-copy legacy fields */
    if (config->www_root[0] == '\0' && config->root[0] != '\0') {
        copy_text(config->www_root, sizeof(config->www_root), config->root);
    }
    if (config->root[0] == '\0' && config->www_root[0] != '\0') {
        copy_text(config->root, sizeof(config->root), config->www_root);
    }
    if (config->user_file[0] == '\0' && config->data_path[0] != '\0') {
        copy_text(config->user_file, sizeof(config->user_file),
                  config->data_path);
    }
    if (config->data_path[0] == '\0' && config->user_file[0] != '\0') {
        copy_text(config->data_path, sizeof(config->data_path),
                  config->user_file);
    }

    /* Apply defaults */
    if (config->max_connections <= 0)        config->max_connections = 256;
    if (config->max_request_bytes <= 0)      config->max_request_bytes = 4096;
    if (config->worker_processes <= 0)       config->worker_processes = 2;
    if (config->worker_shutdown_timeout_ms <= 0)
        config->worker_shutdown_timeout_ms = 3000;
    if (config->log_max_lines <= 0)          config->log_max_lines = 10000;
    if (config->log_max_roll_files <= 0)     config->log_max_roll_files = 5;

    /* System/access log fallback */
    if (config->system_log[0] == '\0') {
        copy_text(config->system_log, sizeof(config->system_log),
                  config->log_path);
    }
    if (config->access_log[0] == '\0') {
        copy_text(config->access_log, sizeof(config->access_log),
                  config->log_path);
    }
}

/* ================================================================
 *  Public API
 * ================================================================ */

int load_config(const char *path, server_config_t *config) {
    FILE *fp;
    int is_nginx;
    int ret;

    if (path == NULL || config == NULL) return -1;

    memset(config, 0, sizeof(*config));

    /* Check if file exists */
    fp = fopen(path, "r");
    if (fp == NULL) return -1;

    /* Detect format */
    is_nginx = has_server_blocks(path);

    if (is_nginx) {
        ret = load_nginx_config(fp, config);
    } else {
        /* Legacy flat format */
        ret = load_legacy_config(fp, config);
        /* Synthesize a single default server block from legacy fields */
        if (ret == 0) {
            server_block_t *sb = &config->servers[0];
            memset(sb, 0, sizeof(*sb));
            copy_text(sb->host, sizeof(sb->host), config->host);
            sb->port = config->port;
            copy_text(sb->root, sizeof(sb->root), config->www_root);
            copy_text(sb->access_log, sizeof(sb->access_log), config->access_log);
            if (config->server_name[0] != '\0') {
                copy_text(sb->server_names[0], MAX_SERVER_NAME_LEN,
                          config->server_name);
                sb->name_count = 1;
            }
            sb->is_default = 1;
            config->server_count = 1;
        }
    }

    fclose(fp);

    if (ret != 0) return -1;

    config_post_process(config);

    /* Validate required fields */
    if (config->host[0] == '\0' ||
        config->port <= 0 ||
        config->www_root[0] == '\0' ||
        config->user_file[0] == '\0') {
        /* For new format, log_path is also required */
        if (config->server_count > 0 && config->log_path[0] == '\0') {
            copy_text(config->log_path, sizeof(config->log_path),
                      "logs/server.log");
        }
        if (config->log_path[0] == '\0') return -1;
        /* But if server blocks provide roots, www_root being empty is OK */
        if (config->server_count > 0) return 0;
        return -1;
    }

    return 0;
}

void print_config(const server_config_t *config) {
    int i, j;

    printf("server_name=%s\n", config->server_name);
    printf("host=%s\n", config->host);
    printf("port=%d\n", config->port);
    printf("www_root=%s\n", config->www_root);
    printf("user_file=%s\n", config->user_file);
    printf("log=%s\n", config->log_path);
    printf("system_log=%s\n", config->system_log);
    printf("access_log=%s\n", config->access_log);
    printf("max_connections=%d\n", config->max_connections);
    printf("max_request_bytes=%d\n", config->max_request_bytes);
    printf("worker_processes=%d\n", config->worker_processes);
    printf("worker_shutdown_timeout_ms=%d\n",
           config->worker_shutdown_timeout_ms);
    printf("log_max_lines=%d\n", config->log_max_lines);
    printf("log_max_roll_files=%d\n", config->log_max_roll_files);
    printf("server_count=%d\n", config->server_count);

    for (i = 0; i < config->server_count; i++) {
        const server_block_t *sb = &config->servers[i];
        printf("server[%d]:\n", i);
        printf("  listen=%s:%d", sb->host, sb->port);
        if (sb->is_default) printf(" (default_server)");
        printf("\n");
        printf("  root=%s\n", sb->root);
        printf("  access_log=%s\n",
               sb->access_log[0] ? sb->access_log : "(global)");
        printf("  server_names=[");
        for (j = 0; j < sb->name_count; j++) {
            if (j > 0) printf(", ");
            printf("%s", sb->server_names[j]);
        }
        printf("]\n");
    }
}
