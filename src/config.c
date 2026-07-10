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
        } else if (strcmp(key, "root") == 0) {
            copy_text(config->root, sizeof(config->root), value);
        } else if (strcmp(key, "log") == 0) {
            copy_text(config->log_path, sizeof(config->log_path), value);
        }
    }

    fclose(fp);

    if (config->server_name[0] == '\0' ||
        config->host[0] == '\0' ||
        config->port <= 0 ||
        config->root[0] == '\0' ||
        config->log_path[0] == '\0') {
        return -1;
    }

    return 0;
}

void print_config(const server_config_t *config) {
    printf("server_name=%s\n", config->server_name);
    printf("host=%s\n", config->host);
    printf("port=%d\n", config->port);
    printf("root=%s\n", config->root);
    printf("log=%s\n", config->log_path);
}
