/*
 * config.c — Server configuration parser v1.2.1
 *
 * Supports two formats:
 *   1. Legacy flat key=value  (auto-detected, backward compatible)
 *   2. Nginx-style server { } blocks with name-based virtual hosting
 */

#include "../include/config.h"
#include "../include/route_table.h"
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

/* ---- v1.5: route table accessor ---- */

route_table_t *config_get_route_table(void) {
    if (g_global_config != NULL) {
        return g_global_config->route_table;
    }
    return NULL;
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
    } else if (strcmp(key, "log_level") == 0) {
        if (strcmp(value, "debug") == 0) config->log_level = LOG_DEBUG;
        else if (strcmp(value, "info") == 0) config->log_level = LOG_INFO;
        else if (strcmp(value, "warning") == 0) config->log_level = LOG_WARNING;
        else if (strcmp(value, "error") == 0) config->log_level = LOG_ERROR;
        else if (strcmp(value, "none") == 0) config->log_level = LOG_NONE;
        else {
            fprintf(stderr, "WARNING: unknown log_level '%s', using 'info'\n", value);
            config->log_level = LOG_INFO;
        }
    } else if (strcmp(key, "error_log") == 0) {
        /* error_log <path> [level] */
        char path_buf[128];
        char level_buf[32];
        int n = 0;
        level_buf[0] = '\0';
        /* sscanf to parse "path [level]" */
        n = sscanf(value, "%127s %31s", path_buf, level_buf);
        if (n >= 1) {
            copy_text(config->error_log, sizeof(config->error_log), path_buf);
            if (n >= 2) {
                if (strcmp(level_buf, "debug") == 0) config->error_log_level = LOG_DEBUG;
                else if (strcmp(level_buf, "info") == 0) config->error_log_level = LOG_INFO;
                else if (strcmp(level_buf, "warning") == 0) config->error_log_level = LOG_WARNING;
                else if (strcmp(level_buf, "error") == 0) config->error_log_level = LOG_ERROR;
                else config->error_log_level = LOG_WARNING;
            } else {
                config->error_log_level = LOG_WARNING;
            }
        }
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

/* ================================================================
 *  v1.5: location { } block parser
 * ================================================================ */

/*
 * Parse a "location ... {" line (already read) and then parse the
 * block interior (handler + methods directives).  Adds the route
 * to config->route_table on success.
 *
 * Parsed forms:
 *   location = /path { ... }   →  MATCH_EXACT
 *   location /path/* { ... }   →  MATCH_PREFIX (trailing wildcard)
 *   location /path { ... }     →  MATCH_EXACT (no =, no *)
 */
static int parse_location_directive(const char *location_line,
                                      FILE *fp, server_config_t *config) {
    char buf[256];
    char path[256];
    match_type_t match_type;
    char *p;

    copy_text(buf, sizeof(buf), location_line);

    /* Remove trailing "{" */
    p = strchr(buf, '{');
    if (p) *p = '\0';
    /* Trim */
    p = trim(buf);

    /* Skip "location" keyword */
    if (strncmp(p, "location", 8) != 0) return -1;
    p += 8;
    while (*p == ' ' || *p == '\t') p++;

    /* Check for "=" (exact match modifier) */
    match_type = MATCH_PREFIX;  /* default */
    if (*p == '=') {
        match_type = MATCH_EXACT;
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* Extract path */
    {
        char path_raw[256];
        int len = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '{') {
            if (len < (int)sizeof(path_raw) - 1) {
                path_raw[len++] = *p;
            }
            p++;
        }
        path_raw[len] = '\0';

        /* Check if path ends with "/*" → MATCH_PREFIX */
        {
            size_t plen = strlen(path_raw);
            if (plen >= 2 && strcmp(path_raw + plen - 2, "/*") == 0) {
                /* Remove the "*", keep the "/" */
                path_raw[plen - 1] = '\0';
                match_type = MATCH_PREFIX;
            } else if (match_type != MATCH_EXACT) {
                /* No = and no * → treat as exact by default for v1.5 */
                match_type = MATCH_EXACT;
            }
        }

        copy_text(path, sizeof(path), path_raw);
    }

    /* Inline the location block parsing here directly. */
    {
        char line[256];
        char methods_buf[256];
        char handler_name[MAX_HANDLER_NAME_LEN];
        handler_type_t htype;
        int has_handler = 0;
        int method_count = 0;
        char methods_storage[8][16];

        handler_name[0] = '\0';

        while (fgets(line, sizeof(line), fp) != NULL) {
            char *text, *separator;

            remove_newline(line);
            text = trim(line);

            if (text[0] == '\0' || text[0] == '#') continue;

            if (text[0] == '}') {
                if (!has_handler) {
                    fprintf(stderr,
                            "ERROR: location '%s' missing handler directive\n",
                            path);
                    return -1;
                }
                htype = route_table_lookup_handler(handler_name);
                if (htype == HANDLER_NONE) {
                    fprintf(stderr,
                            "ERROR: location '%s': unknown handler '%s'\n",
                            path, handler_name);
                    return -1;
                }
                if (method_count == 0) {
                    fprintf(stderr,
                            "ERROR: location '%s': no methods specified\n",
                            path);
                    return -1;
                }
                {
                    int k;
                    /* v1.5: one entry per (method, path) */
                    for (k = 0; k < method_count; k++) {
                        if (route_table_add(config->route_table,
                                            methods_storage[k],
                                            path, match_type, htype) != 0) {
                            return -1;
                        }
                    }
                }
                return 0;
            }

            text = strip_semicolon(text);

            separator = strchr(text, ' ');
            if (separator) {
                *separator = '\0';
                {
                    char *key   = trim(text);
                    char *value = trim(separator + 1);

                    if (strcmp(key, "handler") == 0) {
                        copy_text(handler_name, sizeof(handler_name), value);
                        has_handler = 1;
                    } else if (strcmp(key, "methods") == 0) {
                        char *saveptr, *token;
                        copy_text(methods_buf, sizeof(methods_buf), value);
                        method_count = 0;
                        token = strtok_r(methods_buf, " \t", &saveptr);
                        while (token && method_count < 8) {
                            int mi;
                            strncpy(methods_storage[method_count], token,
                                    sizeof(methods_storage[0]) - 1);
                            methods_storage[method_count][
                                sizeof(methods_storage[0]) - 1] = '\0';
                            for (mi = 0; methods_storage[method_count][mi]; mi++) {
                                methods_storage[method_count][mi] =
                                    (char)toupper(
                                        (unsigned char)methods_storage[method_count][mi]);
                            }
                            method_count++;
                            token = strtok_r(NULL, " \t", &saveptr);
                        }
                    }
                }
            }
        }

        fprintf(stderr, "ERROR: location block not closed (missing '}')\n");
        return -1;
    }
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

        /* v1.5: location block inside server block */
        if (strncmp(text, "location", 8) == 0) {
            if (parse_location_directive(text, fp, config) != 0) {
                return -1;
            }
            continue;
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

        /* v1.5: "location ... {" opens a location block */
        if (strncmp(text, "location", 8) == 0) {
            if (parse_location_directive(text, fp, config) != 0) {
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
 *  v1.5: JSON config parser (minimal — no external dependencies)
 *
 *  Supported structure:
 *  {
 *    "host": "0.0.0.0",
 *    "port": 8080,
 *    "worker_processes": 2,
 *    "www_root": "www",
 *    "user_file": "data/users.csv",
 *    "log_level": "info",
 *    "error_log": "logs/error.log",
 *    "system_log": "logs/system.log",
 *    "access_log": "logs/access.log",
 *    "log_max_lines": 10000,
 *    "log_max_roll_files": 5,
 *    "max_connections": 256,
 *    "max_request_bytes": 4096,
 *    "logging": {
 *      "level": "info",
 *      "file": "logs/server.log",
 *      "format": "[%(timestamp)s] [%(level)s] %(message)s"
 *    },
 *    "servers": [{
 *      "listen": "0.0.0.0:8080",
 *      "server_name": ["localhost"],
 *      "root": "www",
 *      "routes": [
 *        {"method":"GET","path":"/","handler":"index"},
 *        {"method":"GET","path":"/hello","handler":"hello"}
 *      ]
 *    }],
 *    "routes": [
 *      {"method":"GET","path":"/","handler":"index"}
 *    ]
 *  }
 * ================================================================ */

/* ---- minimal JSON helpers ---- */

/* Skip whitespace, return pointer to first non-whitespace char */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Extract a JSON string value (between quotes). Returns dst, or NULL. */
static const char *json_get_string(const char *p, char *dst, int dst_size) {
    p = json_skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    {
        int i = 0;
        while (*p && *p != '"' && i < dst_size - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                    case 'n':  dst[i++] = '\n'; break;
                    case 't':  dst[i++] = '\t'; break;
                    case '\\': dst[i++] = '\\'; break;
                    case '"':  dst[i++] = '"';  break;
                    default:   dst[i++] = *p;   break;
                }
            } else {
                dst[i++] = *p;
            }
            p++;
        }
        dst[i] = '\0';
        if (*p == '"') p++;
        return p;
    }
}

/* Find a key in a JSON object string. Returns pointer to value start. */
static const char *json_find_key(const char *p, const char *key) {
    char kbuf[128];
    p = json_skip_ws(p);
    if (*p != '{') return NULL;
    p++;

    while (*p) {
        p = json_skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        p = json_get_string(p, kbuf, sizeof(kbuf));
        if (!p) break;

        p = json_skip_ws(p);
        if (*p != ':') break;
        p++;
        p = json_skip_ws(p);

        if (strcmp(kbuf, key) == 0) return p;
        /* Skip value */
        if (*p == '"') { char tmp[256]; p = json_get_string(p, tmp, sizeof(tmp)); }
        else if (*p == '{') { int d = 1; p++; while (*p && d) { if (*p == '{') d++; if (*p == '}') d--; p++; } }
        else if (*p == '[') { int d = 1; p++; while (*p && d) { if (*p == '[') d++; if (*p == ']') d--; p++; } }
        else { while (*p && *p != ',' && *p != '}' && *p != '\n') p++; }
    }
    return NULL;
}

/* Parse a JSON integer value */
static int json_get_int(const char *p, int *out) {
    p = json_skip_ws(p);
    if (*p == '"') { /* string-wrapped int */
        char buf[32];
        p = json_get_string(p, buf, sizeof(buf));
        *out = atoi(buf);
        return 0;
    }
    *out = atoi(p);
    return 0;
}

/* Parse a JSON string value into a buffer */
static int json_get_str(const char *p, char *dst, int dst_size) {
    p = json_skip_ws(p);
    if (*p == '"') {
        json_get_string(p, dst, dst_size);
        return 0;
    }
    /* unquoted — copy until comma/brace */
    {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && i < dst_size - 1) {
            dst[i++] = *p++;
        }
        dst[i] = '\0';
    }
    return 0;
}

/* Parse a JSON array of strings, return count */
static int json_get_string_array(const char *p, char arr[][128], int max_count) {
    int count = 0;
    p = json_skip_ws(p);
    if (*p != '[') return 0;
    p++;
    while (*p && count < max_count) {
        p = json_skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        p = json_get_string(p, arr[count], 128);
        count++;
    }
    return count;
}

/* Parse one JSON route object: {"method":"GET","path":"/","handler":"hello"} */
static int json_parse_route(const char *p, route_table_t *rt) {
    char method[16], path[256], handler_name[MAX_HANDLER_NAME_LEN];
    match_type_t match_type;
    handler_type_t htype;
    const char *v;
    size_t plen;

    /* method */
    v = json_find_key(p, "method");
    if (!v) { fprintf(stderr, "ERROR: route missing 'method'\n"); return -1; }
    json_get_str(v, method, sizeof(method));
    /* uppercase */
    { int i; for (i = 0; method[i]; i++) method[i] = (char)toupper((unsigned char)method[i]); }

    /* path */
    v = json_find_key(p, "path");
    if (!v) { fprintf(stderr, "ERROR: route missing 'path'\n"); return -1; }
    json_get_str(v, path, sizeof(path));

    /* handler */
    v = json_find_key(p, "handler");
    if (!v) { fprintf(stderr, "ERROR: route missing 'handler'\n"); return -1; }
    json_get_str(v, handler_name, sizeof(handler_name));

    /* Determine match_type from path */
    plen = strlen(path);
    if (plen >= 2 && strcmp(path + plen - 2, "/*") == 0) {
        path[plen - 1] = '\0';  /* strip "*", keep "/" */
        match_type = MATCH_PREFIX;
    } else {
        match_type = MATCH_EXACT;
    }

    htype = route_table_lookup_handler(handler_name);
    if (htype == HANDLER_NONE) {
        fprintf(stderr, "ERROR: unknown handler '%s' for path '%s'\n",
                handler_name, path);
        return -1;
    }

    return route_table_add(rt, method, path, match_type, htype);
}

/* Parse the "routes" array from JSON */
static int json_parse_routes(const char *p, route_table_t *rt) {
    int count = 0;
    p = json_skip_ws(p);
    if (*p != '[') return 0;
    p++;

    while (*p) {
        p = json_skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p == '{') {
            const char *end;
            int depth = 1;
            end = p + 1;
            while (*end && depth) {
                if (*end == '{') depth++;
                if (*end == '}') depth--;
                end++;
            }
            {
                char obj[1024];
                int len = (int)(end - p);
                if (len > (int)sizeof(obj) - 1) len = (int)sizeof(obj) - 1;
                memcpy(obj, p, len);
                obj[len] = '\0';
                if (json_parse_route(obj, rt) != 0) return -1;
                count++;
            }
            p = end;
        } else {
            p++;
        }
    }
    return count;
}

/* Parse a "servers" array entry */
static int json_parse_server(const char *p, server_config_t *config) {
    server_block_t *sb;
    const char *v;

    if (config->server_count >= MAX_SERVER_BLOCKS) return -1;
    sb = &config->servers[config->server_count];
    memset(sb, 0, sizeof(*sb));
    sb->port = 80;
    copy_text(sb->host, sizeof(sb->host), "0.0.0.0");

    /* listen */
    v = json_find_key(p, "listen");
    if (v) {
        char buf[128]; json_get_str(v, buf, sizeof(buf));
        parse_listen(buf, sb);
    }

    /* server_name */
    v = json_find_key(p, "server_name");
    if (v) sb->name_count = json_get_string_array(v, sb->server_names, MAX_SERVER_NAMES);

    /* root */
    v = json_find_key(p, "root");
    if (v) json_get_str(v, sb->root, sizeof(sb->root));

    /* access_log */
    v = json_find_key(p, "access_log");
    if (v) json_get_str(v, sb->access_log, sizeof(sb->access_log));

    /* routes */
    v = json_find_key(p, "routes");
    if (v) {
        if (json_parse_routes(v, config->route_table) < 0) return -1;
    }

    config->server_count++;
    return 0;
}

static int load_json_config(const char *raw, server_config_t *config) {
    const char *p = raw;
    const char *v;

    /* ---- flat fields ---- */
    v = json_find_key(p, "host");          if (v) json_get_str(v, config->host, sizeof(config->host));
    v = json_find_key(p, "port");          if (v) { int iv; json_get_int(v, &iv); config->port = iv; }
    v = json_find_key(p, "www_root");      if (v) json_get_str(v, config->www_root, sizeof(config->www_root));
    v = json_find_key(p, "user_file");     if (v) json_get_str(v, config->user_file, sizeof(config->user_file));
    v = json_find_key(p, "system_log");    if (v) json_get_str(v, config->system_log, sizeof(config->system_log));
    v = json_find_key(p, "access_log");    if (v) json_get_str(v, config->access_log, sizeof(config->access_log));
    v = json_find_key(p, "log_level");     if (v) { char b[32]; json_get_str(v,b,sizeof(b));
        if (strcmp(b,"debug")==0) config->log_level=LOG_DEBUG; else if (strcmp(b,"info")==0) config->log_level=LOG_INFO;
        else if (strcmp(b,"warning")==0) config->log_level=LOG_WARNING; else if (strcmp(b,"error")==0) config->log_level=LOG_ERROR;
        else if (strcmp(b,"none")==0) config->log_level=LOG_NONE; }
    v = json_find_key(p, "error_log");     if (v) json_get_str(v, config->error_log, sizeof(config->error_log));
    v = json_find_key(p, "log_max_lines"); if (v) { int iv; json_get_int(v, &iv); config->log_max_lines = iv; }
    v = json_find_key(p, "log_max_roll_files"); if (v) { int iv; json_get_int(v, &iv); config->log_max_roll_files = iv; }
    v = json_find_key(p, "worker_processes");    if (v) { int iv; json_get_int(v, &iv); config->worker_processes = iv; }
    v = json_find_key(p, "worker_shutdown_timeout_ms"); if (v) { int iv; json_get_int(v, &iv); config->worker_shutdown_timeout_ms = iv; }
    v = json_find_key(p, "max_connections");   if (v) { int iv; json_get_int(v, &iv); config->max_connections = iv; }
    v = json_find_key(p, "max_request_bytes"); if (v) { int iv; json_get_int(v, &iv); config->max_request_bytes = iv; }

    /* ---- logging object ---- */
    v = json_find_key(p, "logging");
    if (v) {
        const char *lv;
        lv = json_find_key(v, "level");  if (lv) { char b[32]; json_get_str(lv,b,sizeof(b));
            if (strcmp(b,"debug")==0) config->log_level=LOG_DEBUG; else if (strcmp(b,"info")==0) config->log_level=LOG_INFO;
            else if (strcmp(b,"warning")==0) config->log_level=LOG_WARNING; else if (strcmp(b,"error")==0) config->log_level=LOG_ERROR;
            else if (strcmp(b,"none")==0) config->log_level=LOG_NONE; }
        lv = json_find_key(v, "file");  if (lv) json_get_str(lv, config->system_log, sizeof(config->system_log));
        lv = json_find_key(v, "system_log"); if (lv) json_get_str(lv, config->system_log, sizeof(config->system_log));
        lv = json_find_key(v, "access_log"); if (lv) json_get_str(lv, config->access_log, sizeof(config->access_log));
    }

    /* ---- top-level routes array ---- */
    v = json_find_key(p, "routes");
    if (v) {
        if (json_parse_routes(v, config->route_table) < 0) return -1;
    }

    /* ---- servers array ---- */
    v = json_find_key(p, "servers");
    if (v) {
        v = json_skip_ws(v);
        if (*v == '[') {
            v++;
            while (*v) {
                v = json_skip_ws(v);
                if (*v == ']') break;
                if (*v == ',') { v++; continue; }
                if (*v == '{') {
                    const char *end;
                    int depth = 1;
                    end = v + 1;
                    while (*end && depth) { if (*end == '{') depth++; if (*end == '}') depth--; end++; }
                    {
                        char obj[2048];
                        int len = (int)(end - v);
                        if (len > (int)sizeof(obj) - 1) len = (int)sizeof(obj) - 1;
                        memcpy(obj, v, len); obj[len] = '\0';
                        if (json_parse_server(obj, config) != 0) return -1;
                    }
                    v = end;
                } else { v++; }
            }
        }
    }

    return 0;
}

/* ================================================================
 *  v1.5: YAML config parser (minimal subset — indentation-based)
 *
 *  Supported structure:
 *    host: "0.0.0.0"
 *    port: 8080
 *    worker_processes: 2
 *    www_root: www
 *    user_file: data/users.csv
 *    log_level: info
 *    logging:
 *      level: info
 *      file: logs/server.log
 *      format: "[%(timestamp)s] [%(level)s] %(message)s"
 *    servers:
 *      - listen: "0.0.0.0:8080"
 *        server_name:
 *          - localhost
 *        root: www
 *        routes:
 *          - method: GET
 *            path: /
 *            handler: index
 *    routes:
 *      - method: GET
 *        path: /hello
 *        handler: hello
 * ================================================================ */

/* YAML: get indentation level (2 spaces per level) */
static int yaml_indent(const char *line) {
    int n = 0;
    while (*line == ' ') { n++; line++; }
    return n;
}

/* YAML: skip leading "--" or "---" */
static int yaml_is_doc_marker(const char *line) {
    while (*line == ' ') line++;
    return (strncmp(line, "---", 3) == 0 || strncmp(line, "--", 2) == 0);
}

/* YAML: parse a scalar value (quoted or unquoted) */
static void yaml_get_value(const char *line, char *dst, int dst_size) {
    const char *colon = strchr(line, ':');
    if (!colon) { dst[0] = '\0'; return; }
    colon++;
    while (*colon == ' ') colon++;
    if (*colon == '"') {
        json_get_string(colon, dst, dst_size);
    } else if (*colon == '\''){
        colon++;
        int i = 0;
        while (*colon && *colon != '\'' && i < dst_size - 1) dst[i++] = *colon++;
        dst[i] = '\0';
    } else {
        int i = 0;
        while (*colon && *colon != '\n' && *colon != '\r' && *colon != '#' && i < dst_size - 1) {
            dst[i++] = *colon++;
        }
        /* trim trailing space */
        while (i > 0 && dst[i-1] == ' ') i--;
        dst[i] = '\0';
    }
}

/* YAML: check if line is "  - xxx" (list item at given indent) */
static int yaml_is_list_item(const char *line, int indent) {
    int i = 0;
    while (i < indent && line[i] == ' ') i++;
    return (i == indent && line[i] == '-' && line[i+1] == ' ');
}

/* YAML: check if line is a key at given indent */
static int yaml_is_key_at(const char *line, int indent, const char *key) {
    int i = 0;
    while (i < indent && line[i] == ' ') i++;
    if (i != indent) return 0;
    return (strncmp(line + i, key, strlen(key)) == 0 && line[i + strlen(key)] == ':');
}

/* YAML: parse a single route map — base_indent is the list-item indent */
static int yaml_parse_route_map(char **lines, int *idx, int total,
                                 int list_indent, route_table_t *rt) {
    char method[16] = "";
    char path[256] = "";
    char handler_name[MAX_HANDLER_NAME_LEN] = "";

    while (*idx < total) {
        char *l = lines[*idx];
        if (l[0] == '\0' || l[0] == '#') { (*idx)++; continue; }
        int indent = yaml_indent(l);
        /* Stop when we go back to list level or above */
        if (indent <= list_indent && !(l[indent] == '-' && l[indent+1] == ' ')) break;

        const char *content_start = l;
        /* Strip "- " prefix if this line is a list item */
        if (l[indent] == '-' && l[indent+1] == ' ') {
            content_start = l + indent + 2;
        }
        /* Skip leading whitespace */
        while (*content_start == ' ') content_start++;

        if (strchr(content_start, ':')) {
            char key[64] = "";
            char val_buf[256];
            const char *colon = strchr(content_start, ':');
            int klen = (int)(colon - content_start);
            if (klen > 63) klen = 63;
            memcpy(key, content_start, klen); key[klen] = '\0';
            { int ki = (int)strlen(key) - 1; while (ki >= 0 && key[ki] == ' ') key[ki--] = '\0'; }

            yaml_get_value(l, val_buf, sizeof(val_buf));

            if (strcmp(key, "method") == 0) {
                strncpy(method, val_buf, sizeof(method)-1);
                { int i; for(i=0;method[i];i++) method[i]=(char)toupper((unsigned char)method[i]); }
            } else if (strcmp(key, "path") == 0) {
                strncpy(path, val_buf, sizeof(path)-1);
            } else if (strcmp(key, "handler") == 0) {
                strncpy(handler_name, val_buf, sizeof(handler_name)-1);
            }
        }
        (*idx)++;
    }

    if (method[0] && path[0] && handler_name[0]) {
        match_type_t match_type;
        handler_type_t htype;
        size_t plen = strlen(path);
        if (plen >= 2 && strcmp(path + plen - 2, "/*") == 0) {
            path[plen - 1] = '\0';
            match_type = MATCH_PREFIX;
        } else {
            match_type = MATCH_EXACT;
        }
        htype = route_table_lookup_handler(handler_name);
        if (htype == HANDLER_NONE) {
            fprintf(stderr, "ERROR: unknown handler '%s' for '%s'\n", handler_name, path);
            return -1;
        }
        return route_table_add(rt, method, path, match_type, htype);
    }
    return 0;
}

/* YAML: parse a routes: list */
static int yaml_parse_routes(char **lines, int *idx, int total,
                              int base_indent, route_table_t *rt) {
    int list_indent = base_indent + 2;  /* "routes:" → indent 0, "- xxx" at indent 2 */
    while (*idx < total) {
        char *l = lines[*idx];
        if (l[0] == '\0' || l[0] == '#' || yaml_is_doc_marker(l)) { (*idx)++; continue; }
        int indent = yaml_indent(l);
        if (indent < base_indent) break;  /* went above list level */
        if (indent == base_indent && !yaml_is_list_item(l, list_indent)) break;

        if (yaml_is_list_item(l, list_indent)) {
            /* Save current line index for the route map parser */
            if (yaml_parse_route_map(lines, idx, total, list_indent, rt) != 0)
                return -1;
        } else {
            (*idx)++;
        }
    }
    return 0;
}

/* YAML: parse a server_name list */
static int yaml_parse_string_list(char **lines, int *idx, int total,
                                   int base_indent,
                                   char arr[][128], int max_count) {
    int count = 0;
    int item_indent = base_indent + 2;
    while (*idx < total) {
        char *l = lines[*idx];
        if (l[0] == '\0' || l[0] == '#') { (*idx)++; continue; }
        int indent = yaml_indent(l);
        if (indent <= base_indent) break;
        if (yaml_is_list_item(l, item_indent) && count < max_count) {
            yaml_get_value(l, arr[count], 128);
            count++;
        }
        (*idx)++;
    }
    return count;
}

/* YAML: parse a key:value from a line, handling "- key: value" list items */
static int yaml_parse_key_value(const char *line, int indent, char *key, int key_size, char *value, int val_size) {
    const char *content = line;
    /* Strip leading indentation */
    while (*content == ' ') content++;
    /* Strip "- " prefix if list item */
    if (*content == '-' && content[1] == ' ') content += 2;
    /* Find colon */
    const char *colon = strchr(content, ':');
    if (!colon) return -1;
    int klen = (int)(colon - content);
    if (klen >= key_size) klen = key_size - 1;
    memcpy(key, content, klen); key[klen] = '\0';
    /* Trim trailing space from key */
    { int ki = klen - 1; while (ki >= 0 && key[ki] == ' ') key[ki--] = '\0'; }
    /* Get value from full line (using original function) */
    yaml_get_value(line, value, val_size);
    return 0;
}

/* YAML: parse one server block */
static int yaml_parse_server(char **lines, int *idx, int total,
                              int base_indent, server_config_t *config) {
    server_block_t *sb;
    if (config->server_count >= MAX_SERVER_BLOCKS) return -1;
    sb = &config->servers[config->server_count];
    memset(sb, 0, sizeof(*sb));
    sb->port = 80;
    copy_text(sb->host, sizeof(sb->host), "0.0.0.0");

    while (*idx < total) {
        char *l = lines[*idx];
        if (l[0] == '\0' || l[0] == '#') { (*idx)++; continue; }
        int indent = yaml_indent(l);
        if (indent < base_indent) break;
        if (indent == base_indent && !(l[indent] == '-' && l[indent+1] == ' ')) break;

        char key[64], value[256];
        if (yaml_parse_key_value(l, indent, key, sizeof(key), value, sizeof(value)) == 0) {
            if (strcmp(key, "listen") == 0) {
                parse_listen(value, sb);
            } else if (strcmp(key, "server_name") == 0) {
                (*idx)++;
                sb->name_count = yaml_parse_string_list(lines, idx, total, indent, sb->server_names, MAX_SERVER_NAMES);
                continue;
            } else if (strcmp(key, "root") == 0) {
                copy_text(sb->root, sizeof(sb->root), value);
            } else if (strcmp(key, "access_log") == 0) {
                copy_text(sb->access_log, sizeof(sb->access_log), value);
            } else if (strcmp(key, "routes") == 0) {
                (*idx)++;
                if (yaml_parse_routes(lines, idx, total, indent, config->route_table) != 0) return -1;
                continue;
            }
        }
        (*idx)++;
    }

    config->server_count++;
    return 0;
}

static int load_yaml_config(char **lines, int total, server_config_t *config) {
    int idx = 0;

    while (idx < total) {
        char *l = lines[idx];
        if (l[0] == '\0' || l[0] == '#' || yaml_is_doc_marker(l)) { idx++; continue; }

        int indent = yaml_indent(l);

        if (indent == 0) {
            char key[64];
            const char *colon = strchr(l, ':');
            if (!colon) { idx++; continue; }
            {
                int klen = (int)(colon - l);
                if (klen > 63) klen = 63;
                memcpy(key, l, klen); key[klen] = '\0';
            }

            if (strcmp(key, "host") == 0) {
                char buf[128]; yaml_get_value(l, buf, sizeof(buf));
                copy_text(config->host, sizeof(config->host), buf);
            } else if (strcmp(key, "port") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->port = atoi(buf);
            } else if (strcmp(key, "www_root") == 0) {
                char buf[128]; yaml_get_value(l, buf, sizeof(buf));
                copy_text(config->www_root, sizeof(config->www_root), buf);
            } else if (strcmp(key, "user_file") == 0) {
                char buf[128]; yaml_get_value(l, buf, sizeof(buf));
                copy_text(config->user_file, sizeof(config->user_file), buf);
            } else if (strcmp(key, "system_log") == 0) {
                char buf[128]; yaml_get_value(l, buf, sizeof(buf));
                copy_text(config->system_log, sizeof(config->system_log), buf);
            } else if (strcmp(key, "access_log") == 0) {
                char buf[128]; yaml_get_value(l, buf, sizeof(buf));
                copy_text(config->access_log, sizeof(config->access_log), buf);
            } else if (strcmp(key, "log_level") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                if (strcmp(buf,"debug")==0) config->log_level=LOG_DEBUG;
                else if (strcmp(buf,"info")==0) config->log_level=LOG_INFO;
                else if (strcmp(buf,"warning")==0) config->log_level=LOG_WARNING;
                else if (strcmp(buf,"error")==0) config->log_level=LOG_ERROR;
                else if (strcmp(buf,"none")==0) config->log_level=LOG_NONE;
            } else if (strcmp(key, "error_log") == 0) {
                char buf[128]; yaml_get_value(l, buf, sizeof(buf));
                copy_text(config->error_log, sizeof(config->error_log), buf);
            } else if (strcmp(key, "log_max_lines") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->log_max_lines = atoi(buf);
            } else if (strcmp(key, "log_max_roll_files") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->log_max_roll_files = atoi(buf);
            } else if (strcmp(key, "worker_processes") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->worker_processes = atoi(buf);
            } else if (strcmp(key, "worker_shutdown_timeout_ms") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->worker_shutdown_timeout_ms = atoi(buf);
            } else if (strcmp(key, "max_connections") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->max_connections = atoi(buf);
            } else if (strcmp(key, "max_request_bytes") == 0) {
                char buf[32]; yaml_get_value(l, buf, sizeof(buf));
                config->max_request_bytes = atoi(buf);
            } else if (strcmp(key, "logging") == 0) {
                idx++;
                while (idx < total) {
                    char *sl = lines[idx];
                    if (sl[0] == '\0' || sl[0] == '#') { idx++; continue; }
                    if (yaml_indent(sl) <= 2) break;
                    if (yaml_is_key_at(sl, 4, "level")) {
                        char buf[32]; yaml_get_value(sl, buf, sizeof(buf));
                        if (strcmp(buf,"debug")==0) config->log_level=LOG_DEBUG;
                        else if (strcmp(buf,"info")==0) config->log_level=LOG_INFO;
                        else if (strcmp(buf,"warning")==0) config->log_level=LOG_WARNING;
                        else if (strcmp(buf,"error")==0) config->log_level=LOG_ERROR;
                        else if (strcmp(buf,"none")==0) config->log_level=LOG_NONE;
                    } else if (yaml_is_key_at(sl, 4, "file") || yaml_is_key_at(sl, 4, "system_log")) {
                        char buf[128]; yaml_get_value(sl, buf, sizeof(buf));
                        copy_text(config->system_log, sizeof(config->system_log), buf);
                    } else if (yaml_is_key_at(sl, 4, "access_log")) {
                        char buf[128]; yaml_get_value(sl, buf, sizeof(buf));
                        copy_text(config->access_log, sizeof(config->access_log), buf);
                    }
                    idx++;
                }
                continue;
            } else if (strcmp(key, "routes") == 0) {
                idx++;
                if (yaml_parse_routes(lines, &idx, total, 0, config->route_table) != 0) return -1;
                continue;
            } else if (strcmp(key, "servers") == 0) {
                idx++;
                while (idx < total) {
                    char *sl = lines[idx];
                    if (sl[0] == '\0' || sl[0] == '#') { idx++; continue; }
                    if (yaml_indent(sl) <= 0) break;
                    if (yaml_is_list_item(sl, 2)) {
                        if (yaml_parse_server(lines, &idx, total, 2, config) != 0) return -1;
                        continue;
                    }
                    idx++;
                }
                continue;
            }
        }
        idx++;
    }

    return 0;
}

/* ================================================================
 *  Format detection
 * ================================================================ */

typedef enum { FMT_NGINX, FMT_JSON, FMT_YAML } config_format_t;

/*
 * Detect configuration file format with priority: Nginx > JSON > YAML.
 *
 * Detection strategy (stop at first definite match):
 *   1. "server {" anywhere        → Nginx (definite)
 *   2. "location " anywhere       → Nginx (definite)
 *   3. Line starts with "{"       → JSON (definite, but checked after Nginx signals)
 *   4. "---" doc marker           → YAML (definite, lowest priority)
 *
 * Evidence gathering (for ambiguous cases):
 *   - has_colon:   lines with "key: value" pattern (YAML or legacy Nginx key=value)
 *   - has_brace:   lines with { or } (JSON or Nginx blocks)
 *   - has_assign:  lines with "key value" (space-separated, Nginx)
 *
 * Final decision (priority: Nginx > JSON > YAML):
 *   - Any Nginx signal (has_assign, has_brace without colon)
 *     or no clear signal → Nginx
 *   - Clear JSON: has_brace, no colons, no assigns → JSON
 *   - Clear YAML: has_colon, no braces, no assigns → YAML
 */
static config_format_t detect_config_format(const char *path) {
    FILE *fp = fopen(path, "r");
    char first[256];
    int has_brace = 0, has_colon = 0, has_assign = 0;
    int line_count = 0;

    if (!fp) return FMT_NGINX;

    while (fgets(first, sizeof(first), fp) && line_count < 50) {
        char *t = first;
        while (*t == ' ' || *t == '\t') t++;
        if (t[0] == '#' || t[0] == '\n' || t[0] == '\r' || t[0] == '\0') {
            line_count++; continue;
        }

        /* ---- Definite Nginx signals (highest priority) ---- */
        if (strncmp(t, "server", 6) == 0 && strchr(t, '{')) {
            fclose(fp); return FMT_NGINX;
        }
        if (strncmp(t, "location", 8) == 0) {
            fclose(fp); return FMT_NGINX;
        }

        /* ---- Definite YAML signal (lowest priority) ---- */
        if (strncmp(t, "---", 3) == 0) {
            fclose(fp); return FMT_YAML;
        }

        /* ---- Gather evidence ---- */
        /* Lines with colons suggest YAML (or legacy Nginx key=value) */
        if (strchr(t, ':') && t[0] != '}') has_colon = 1;

        /* Lines with braces suggest JSON or Nginx blocks */
        if (strchr(t, '{') || strchr(t, '}')) has_brace = 1;

        /* Lines with space-separated key value suggest Nginx (e.g. "worker_processes 2") */
        {
            char *sp = strchr(t, ' ');
            if (sp && !strchr(t, ':') && !strchr(t, '{') && !strchr(t, '}')) {
                char *key_end = sp;
                /* Check there's actually a value after the space */
                while (*sp == ' ' || *sp == '\t') sp++;
                if (*sp && *sp != '\n' && *sp != '\r' && *sp != '#') {
                    has_assign = 1;
                }
            }
        }

        line_count++;
    }
    fclose(fp);

    /*
     * Priority decision: Nginx > JSON > YAML
     *
     * has_assign (Nginx)        → Nginx wins always
     * has_brace + no colon      → JSON (pure JSON object)
     * has_colon + no brace
     *   + no assign             → YAML
     * has_colon + has_brace
     *   + no assign             → JSON (JSON with colons is normal)
     * anything else or ambiguous → Nginx (default, highest priority)
     */

    /* Nginx: space-separated directives are very characteristic */
    if (has_assign) return FMT_NGINX;

    /* JSON: braces without YAML-style colons at indent 0 */
    if (has_brace && !has_colon) return FMT_JSON;

    /* JSON: has braces, may have colons (normal JSON), but no assign */
    if (has_brace) return FMT_JSON;

    /* YAML: colons, no braces, no assigns */
    if (has_colon && !has_brace && !has_assign) return FMT_YAML;

    /* Default: Nginx (highest priority when ambiguous) */
    return FMT_NGINX;
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

    /* v1.5: log_level defaults */
    if (config->log_level < LOG_DEBUG || config->log_level > LOG_NONE) {
        config->log_level = LOG_INFO;
    }
    if (config->error_log_level < LOG_DEBUG ||
        config->error_log_level > LOG_NONE) {
        config->error_log_level = LOG_WARNING;
    }
}

/* ================================================================
 *  Public API
 * ================================================================ */

int load_config(const char *path, server_config_t *config) {
    FILE *fp;
    int ret;

    if (path == NULL || config == NULL) return -1;

    memset(config, 0, sizeof(*config));

    /* v1.5: allocate and initialize route table */
    {
        route_table_t *rt = (route_table_t *)malloc(sizeof(route_table_t));
        if (rt == NULL) {
            fprintf(stderr, "ERROR: failed to allocate route table\n");
            return -1;
        }
        route_table_init(rt);
        config->route_table = rt;
    }

    /* Check if file exists */
    fp = fopen(path, "r");
    if (fp == NULL) return -1;

    /* v1.5: auto-detect config format (Nginx, JSON, YAML) */
    {
        config_format_t fmt = detect_config_format(path);

        switch (fmt) {
        case FMT_JSON: {
            /* Read entire JSON file into memory */
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
                fprintf(stderr, "ERROR: JSON config file too large\n");
                fclose(fp); return -1;
            }
            char *raw = (char *)malloc((size_t)fsize + 1);
            if (!raw) { fclose(fp); return -1; }
            rewind(fp);
            fread(raw, 1, (size_t)fsize, fp);
            raw[fsize] = '\0';
            ret = load_json_config(raw, config);
            free(raw);
            break;
        }
        case FMT_YAML: {
            /* Read file line by line into array */
            char line_buf[512];
            char *yaml_lines[500];
            int yaml_total = 0;
            rewind(fp);
            while (fgets(line_buf, sizeof(line_buf), fp) && yaml_total < 500) {
                size_t llen = strlen(line_buf);
                /* trim trailing newline */
                if (llen > 0 && (line_buf[llen-1] == '\n' || line_buf[llen-1] == '\r'))
                    line_buf[--llen] = '\0';
                if (llen > 0 && (line_buf[llen-1] == '\r'))
                    line_buf[--llen] = '\0';
                char *copy = (char *)malloc(llen + 1);
                if (copy) { memcpy(copy, line_buf, llen + 1); yaml_lines[yaml_total++] = copy; }
            }
            ret = load_yaml_config(yaml_lines, yaml_total, config);
            { int i; for (i = 0; i < yaml_total; i++) free(yaml_lines[i]); }
            break;
        }
        case FMT_NGINX:
        default:
            ret = load_nginx_config(fp, config);
            break;
        }
    }

    fclose(fp);

    if (ret != 0) return -1;

    config_post_process(config);

    /* v1.5: validate route table before accepting config */
    if (config->route_table != NULL) {
        /*
         * If no routes were configured, populate defaults for backward
         * compatibility (handled in request_handler.c).
         * If routes were configured, validate them.
         */
        if (config->route_table->exact_count + config->route_table->prefix_count > 0) {
            if (route_table_validate(config->route_table) != 0) {
                fprintf(stderr, "ERROR: route table validation failed\n");
                free(config->route_table);
                config->route_table = NULL;
                return -1;
            }
        }
    }

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
    printf("log_level=%d\n", (int)config->log_level);
    printf("error_log=%s (level=%d)\n", config->error_log,
           (int)config->error_log_level);
    printf("server_count=%d\n", config->server_count);
    printf("route_count=%d\n",
           config->route_table ? (config->route_table->exact_count + config->route_table->prefix_count) : 0);

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
