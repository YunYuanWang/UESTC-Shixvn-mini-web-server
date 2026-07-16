#include "../include/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void copy_text(char *dest, int dest_size, const char *src) {
    if (dest_size <= 0) {
        return;
    }

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

int load_config(const char *path, server_config_t *config) {
    FILE *fp;
    char line[256];

    if (path == NULL || config == NULL) {
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key;
        char *value;
        char *separator;

        remove_newline(line);
        key = trim(line);

        if (key[0] == '\0' || key[0] == '#') {
            continue;
        }

        separator = strchr(key, '=');
        if (separator == NULL) {
            fclose(fp);
            return -1;
        }

        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);

        if (key[0] == '\0' || value[0] == '\0') {
            fclose(fp);
            return -1;
        }

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
        } else if (strcmp(key, "max_connections") == 0) {
            config->max_connections = atoi(value);
        } else if (strcmp(key, "max_request_bytes") == 0) {
            config->max_request_bytes = atoi(value);
        }
    }

    fclose(fp);

    /*
     * Backward compatibility: if new-style keys (www_root, user_file)
     * are not set, fall back to legacy keys (root, data).
     * Also copy new-style → legacy so code using either field works.
     */
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

    /*
     * Apply default values for optional fields.
     */
    if (config->max_connections <= 0) {
        config->max_connections = 256;
    }
    if (config->max_request_bytes <= 0) {
        config->max_request_bytes = 4096;
    }

    if (config->host[0] == '\0' ||
        config->port <= 0 ||
        config->www_root[0] == '\0' ||
        config->user_file[0] == '\0' ||
        config->log_path[0] == '\0') {
        return -1;
    }

    return 0;
}

void print_config(const server_config_t *config) {
    printf("server_name=%s\n", config->server_name);
    printf("host=%s\n", config->host);
    printf("port=%d\n", config->port);
    printf("www_root=%s\n", config->www_root);
    printf("user_file=%s\n", config->user_file);
    printf("log=%s\n", config->log_path);
    printf("max_connections=%d\n", config->max_connections);
    printf("max_request_bytes=%d\n", config->max_request_bytes);
}
