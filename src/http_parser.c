/*
 * http_parser.c — HTTP/1.x request parser and response builder (v1.1)
 *
 * Parsing: request line → headers → body
 * Response: dynamic Content-Type, keep-alive negotiation, static file serving
 */

#include "../include/http_parser.h"
#include "../include/log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

/* ---- utility: case-insensitive compare ---- */
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

/* ================================================================
 *  http_request_init
 * ================================================================ */
void http_request_init(http_request_t *req) {
    if (!req) return;
    memset(req, 0, sizeof(*req));
    req->version_major  = 1;
    req->version_minor  = 1;
    req->content_length = -1;   /* absent until parsed */
    req->keep_alive     = 1;    /* HTTP/1.1 default */
    req->body_len       = 0;
    req->body_expected  = 0;
}

/* ================================================================
 *  http_parse_request_line
 * ================================================================ */
int http_parse_request_line(char *line, http_request_t *req) {
    char *p, *save;

    if (!line || !req) return -1;

    /* trim trailing \r */
    p = strchr(line, '\r');
    if (p) *p = '\0';

    /* METHOD */
    p = strchr(line, ' ');
    if (!p) return -1;
    *p = '\0';
    strncpy(req->method, line, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';

    /* URI */
    p++;
    while (*p == ' ') p++;
    save = p;
    p = strchr(p, ' ');
    if (p) {
        *p = '\0';
        p++;
    }
    strncpy(req->uri, save, sizeof(req->uri) - 1);
    req->uri[sizeof(req->uri) - 1] = '\0';

    /* HTTP version (optional) */
    if (p && *p) {
        while (*p == ' ') p++;
        if (strncmp(p, "HTTP/", 5) == 0) {
            p += 5;
            req->version_major = atoi(p);
            p = strchr(p, '.');
            if (p) {
                req->version_minor = atoi(p + 1);
            }
        }
    }

    return 0;
}

/* ================================================================
 *  http_parse_headers
 * ================================================================ */
int http_parse_headers(const char *raw, int raw_len, http_request_t *req) {
    const char *p, *end, *line_start;
    int in_headers;

    if (!raw || !req) return -1;

    p = raw;
    end = raw + raw_len;

    /* Skip request line (find first \r\n) */
    while (p < end - 1 && !(p[0] == '\r' && p[1] == '\n')) p++;
    if (p < end - 1) p += 2;  /* skip \r\n */

    in_headers = 1;
    while (in_headers && p < end - 1) {
        /* Empty line (\r\n) terminates headers */
        if (p[0] == '\r' && p[1] == '\n') {
            break;
        }

        line_start = p;

        /* Find end of this header line */
        while (p < end - 1 && !(p[0] == '\r' && p[1] == '\n')) p++;
        if (p >= end - 1) break;  /* incomplete */

        /* Parse "Name: Value" */
        {
            char line[512];
            int line_len = (int)(p - line_start);
            char *colon;

            if (line_len >= (int)sizeof(line)) line_len = (int)sizeof(line) - 1;
            memcpy(line, line_start, line_len);
            line[line_len] = '\0';

            colon = strchr(line, ':');
            if (colon && req->header_count < HTTP_MAX_HEADERS) {
                char *name  = line;
                char *value = colon + 1;
                http_header_t *h;

                *colon = '\0';

                /* trim name */
                while (*name == ' ' || *name == '\t') name++;
                {
                    char *e = name + strlen(name) - 1;
                    while (e > name && (*e == ' ' || *e == '\t')) {
                        *e = '\0'; e--;
                    }
                }

                /* trim value */
                while (*value == ' ' || *value == '\t') value++;
                {
                    char *e = value + strlen(value) - 1;
                    while (e > value && (*e == ' ' || *e == '\t' || *e == '\r')) {
                        *e = '\0'; e--;
                    }
                }

                h = &req->headers[req->header_count++];
                strncpy(h->name,  name,  sizeof(h->name) - 1);
                h->name[sizeof(h->name) - 1] = '\0';
                strncpy(h->value, value, sizeof(h->value) - 1);
                h->value[sizeof(h->value) - 1] = '\0';

                /* Extract Content-Length */
                if (strcasecmp_safe(h->name, "content-length") == 0) {
                    req->content_length = atoi(h->value);
                    if (req->content_length < 0) req->content_length = 0;
                }

                /* Track Connection header presence */
                if (strcasecmp_safe(h->name, "connection") == 0) {
                    if (strcasecmp_safe(h->value, "close") == 0) {
                        req->keep_alive = 0;
                    } else if (strcasecmp_safe(h->value, "keep-alive") == 0) {
                        req->keep_alive = 1;
                    }
                }
            }
        }

        p += 2;  /* skip \r\n */
    }

    /* ---- keep-alive negotiation based on HTTP version ---- */
    if (req->version_major == 1 && req->version_minor == 0) {
        /* HTTP/1.0: default close unless Connection: keep-alive explicitly set */
        const char *conn = http_get_header(req, "connection");
        if (!conn || strcasecmp_safe(conn, "keep-alive") != 0) {
            req->keep_alive = 0;
        }
    }
    /* HTTP/1.1: default keep-alive, unless Connection: close */
    /* (keep_alive was initialized to 1, and set to 0 above if Connection: close) */

    return 0;
}

/* ================================================================
 *  http_get_header
 * ================================================================ */
const char *http_get_header(const http_request_t *req, const char *name) {
    int i;
    if (!req || !name) return NULL;
    for (i = 0; i < req->header_count; i++) {
        if (strcasecmp_safe(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/* ================================================================
 *  http_parse_body
 * ================================================================ */
int http_parse_body(const char *raw, int raw_len, http_request_t *req) {
    const char *body_start;
    int body_available, to_copy;

    if (!raw || !req) return -1;

    /* Find body start (\r\n\r\n) */
    body_start = strstr(raw, "\r\n\r\n");
    if (!body_start) return 0;  /* no body */
    body_start += 4;

    body_available = (int)(raw + raw_len - body_start);
    if (body_available <= 0) return 0;

    /* Determine how much to read */
    if (req->content_length > 0) {
        req->body_expected = req->content_length;
        to_copy = body_available;
        if (to_copy > req->body_expected) to_copy = req->body_expected;
        if (to_copy > HTTP_MAX_BODY - 1) to_copy = HTTP_MAX_BODY - 1;
    } else {
        /* No Content-Length — read what's there (legacy behavior) */
        to_copy = body_available;
        if (to_copy > HTTP_MAX_BODY - 1) to_copy = HTTP_MAX_BODY - 1;
    }

    memcpy(req->body, body_start, to_copy);
    req->body[to_copy] = '\0';
    req->body_len = to_copy;

    /* Strip trailing \r\n from body */
    while (req->body_len > 0 &&
           (req->body[req->body_len - 1] == '\r' ||
            req->body[req->body_len - 1] == '\n')) {
        req->body[--req->body_len] = '\0';
    }

    return to_copy;
}

/* ================================================================
 *  http_parse_request (one-shot: request line + headers + body)
 * ================================================================ */
int http_parse_request(char *raw, int raw_len, http_request_t *req) {
    char first_line[512];
    char *crlf;

    if (!raw || !req || raw_len <= 0) return -1;

    http_request_init(req);

    /* Extract first line */
    crlf = strstr(raw, "\r\n");
    if (!crlf) {
        /* Try \n only */
        crlf = strchr(raw, '\n');
        if (!crlf) return -1;
    }

    {
        int line_len = (int)(crlf - raw);
        if (line_len >= (int)sizeof(first_line)) line_len = (int)sizeof(first_line) - 1;
        memcpy(first_line, raw, line_len);
        first_line[line_len] = '\0';

        if (http_parse_request_line(first_line, req) != 0) return -1;
    }

    /* Parse headers */
    if (http_parse_headers(raw, raw_len, req) != 0) return -1;

    /* Parse body */
    {
        int result = http_parse_body(raw, raw_len, req);
        if (result < 0) return -1;
    }

    return 0;
}

/* ================================================================
 *  http_content_type_from_path
 * ================================================================ */
const char *http_content_type_from_path(const char *path) {
    const char *ext;

    if (!path) return "text/plain; charset=utf-8";

    ext = strrchr(path, '.');
    if (!ext) return "text/plain; charset=utf-8";

    if (strcasecmp_safe(ext, ".html") == 0 || strcasecmp_safe(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp_safe(ext, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcasecmp_safe(ext, ".js") == 0)
        return "application/javascript";
    if (strcasecmp_safe(ext, ".json") == 0)
        return "application/json";
    if (strcasecmp_safe(ext, ".png") == 0)
        return "image/png";
    if (strcasecmp_safe(ext, ".jpg") == 0 || strcasecmp_safe(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp_safe(ext, ".gif") == 0)
        return "image/gif";
    if (strcasecmp_safe(ext, ".svg") == 0)
        return "image/svg+xml";
    if (strcasecmp_safe(ext, ".ico") == 0)
        return "image/x-icon";
    if (strcasecmp_safe(ext, ".xml") == 0)
        return "application/xml";
    if (strcasecmp_safe(ext, ".pdf") == 0)
        return "application/pdf";

    return "text/plain; charset=utf-8";
}

/* ================================================================
 *  http_serve_file
 * ================================================================ */
int http_serve_file(const char *www_root, const char *uri,
                    char *buf, int buf_size, const char **content_type) {
    char filepath[512];
    FILE *fp;
    long file_size;

    if (!www_root || !uri || !buf || buf_size <= 0) return -1;

    /* Map URI to file path */
    if (strcmp(uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", www_root);
    } else {
        /* Remove leading / and prepend www_root */
        const char *path = uri;
        while (*path == '/') path++;
        snprintf(filepath, sizeof(filepath), "%s/%s", www_root, path);
    }

    /* Set content type before reading */
    if (content_type) {
        *content_type = http_content_type_from_path(filepath);
    }

    /* Open file */
    fp = fopen(filepath, "rb");
    if (!fp) return -1;

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(fp);
        return -1;
    }

    if (file_size >= buf_size) {
        /* File too large for buffer */
        fclose(fp);
        return -1;
    }

    /* Read file content */
    {
        size_t nread = fread(buf, 1, (size_t)file_size, fp);
        fclose(fp);
        buf[nread] = '\0';
        return (int)nread;
    }
}

/* ================================================================
 *  http_build_response
 * ================================================================ */
int http_build_response(int status, const char *status_text,
                        const char *content_type,
                        const char *body, int body_len,
                        int keep_alive,
                        char *output, int size) {
    char date_buf[64];
    time_t now;
    struct tm tm_info;
    int header_len;

    if (!output || size <= 0) return -1;
    if (!body) { body = ""; body_len = 0; }
    if (!content_type) content_type = "text/plain; charset=utf-8";
    if (!status_text) status_text = "OK";

    /* Build HTTP-date for Date header */
    now = time(NULL);
    gmtime_r(&now, &tm_info);
    strftime(date_buf, sizeof(date_buf),
             "%a, %d %b %Y %H:%M:%S GMT", &tm_info);

    header_len = snprintf(output, (size_t)size,
        "HTTP/1.1 %d %s\r\n"
        "Server: MiniWeb/1.2.1\r\n"
        "Date: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, status_text,
        date_buf,
        content_type,
        body_len,
        keep_alive ? "Keep-Alive" : "close");

    if (header_len < 0 || header_len >= size) return -1;

    /* Append body */
    {
        int remaining = size - header_len;
        if (body_len >= remaining) return -1;
        memcpy(output + header_len, body, body_len);
        output[header_len + body_len] = '\0';
    }

    return header_len + body_len;
}

/* ================================================================
 *  http_build_redirect — 301/302 redirect response
 * ================================================================ */
int http_build_redirect(int status, const char *location,
                        char *output, int size) {
    char body[512];
    int body_len;

    if (!output || size <= 0 || !location) return -1;

    body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>%d Redirect</title></head>\n"
        "<body>\n"
        "<h1>%d Moved</h1>\n"
        "<p>The document has moved to <a href=\"%s\">%s</a>.</p>\n"
        "</body>\n"
        "</html>\n",
        status, status, location, location);

    /* Build response with Location header */
    {
        char date_buf[64];
        time_t now = time(NULL);
        struct tm tm_info;
        int header_len;

        gmtime_r(&now, &tm_info);
        strftime(date_buf, sizeof(date_buf),
                 "%a, %d %b %Y %H:%M:%S GMT", &tm_info);

        header_len = snprintf(output, (size_t)size,
            "HTTP/1.1 %d %s\r\n"
            "Server: MiniWeb/1.2.1\r\n"
            "Date: %s\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %d\r\n"
            "Location: %s\r\n"
            "Connection: Keep-Alive\r\n"
            "\r\n",
            status,
            status == 301 ? "MOVED PERMANENTLY" : "FOUND",
            date_buf,
            body_len,
            location);

        if (header_len < 0 || header_len >= size) return -1;

        {
            int remaining = size - header_len;
            if (body_len >= remaining) return -1;
            memcpy(output + header_len, body, body_len);
            output[header_len + body_len] = '\0';
        }

        return header_len + body_len;
    }
}

/* ================================================================
 *  http_is_safe_path — check for directory traversal attacks
 * ================================================================ */
int http_is_safe_path(const char *uri) {
    if (!uri) return 0;

    /* reject paths containing ".." (directory traversal) */
    if (strstr(uri, "..")) return 0;

    /* reject paths with null bytes */
    {
        const char *p;
        for (p = uri; *p; p++) {
            if (*p == '\0') return 0;
        }
    }

    /* reject paths starting with ~ or containing backslashes */
    if (uri[0] == '~') return 0;
    if (strchr(uri, '\\')) return 0;

    return 1;
}

/* ================================================================
 *  http_get_file_mtime — get file last modification time
 * ================================================================ */
time_t http_get_file_mtime(const char *filepath) {
    struct stat st;
    if (!filepath) return 0;
    if (stat(filepath, &st) != 0) return 0;
    return st.st_mtime;
}
