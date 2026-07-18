#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/*
 * http_parser.h — HTTP/1.x request parser (v1.1)
 *
 * Parses the three parts of an HTTP request:
 *   1. Request line:   METHOD /uri HTTP/1.x
 *   2. Headers:        Name: Value (Content-Length, Connection, etc.)
 *   3. Body:           Based on Content-Length header
 *
 * Keep-alive negotiation follows HTTP/1.1 spec:
 *   HTTP/1.0 + no Connection: keep-alive header → close after response
 *   HTTP/1.0 + Connection: keep-alive            → keep alive
 *   HTTP/1.1 + no Connection: close header        → keep alive (default)
 *   HTTP/1.1 + Connection: close                   → close after response
 */

#define HTTP_MAX_HEADERS      32
#define HTTP_MAX_HEADER_NAME  64
#define HTTP_MAX_HEADER_VALUE 256
#define HTTP_MAX_URI         1024
#define HTTP_MAX_BODY        4096

typedef struct {
    char name[HTTP_MAX_HEADER_NAME];
    char value[HTTP_MAX_HEADER_VALUE];
} http_header_t;

typedef struct {
    /* ---- request line ---- */
    char method[16];
    char uri[HTTP_MAX_URI];
    int  version_major;         /* 1 */
    int  version_minor;         /* 0 or 1 */

    /* ---- parsed headers ---- */
    http_header_t headers[HTTP_MAX_HEADERS];
    int  header_count;

    /* ---- derived from headers ---- */
    int  content_length;        /* from Content-Length header, -1 = absent, 0 = no body */
    int  keep_alive;            /* 1 = keep connection, 0 = close */

    /* ---- body ---- */
    char body[HTTP_MAX_BODY];
    int  body_len;              /* bytes written to body[] so far */
    int  body_expected;         /* total expected (from content_length), 0 = no body */
} http_request_t;

/* ---- lifecycle ---- */
void http_request_init(http_request_t *req);

/* ---- request line ---- */
int http_parse_request_line(char *line, http_request_t *req);

/* ---- headers ---- */
int http_parse_headers(const char *raw, int raw_len, http_request_t *req);

/* ---- header lookup (case-insensitive) ---- */
const char *http_get_header(const http_request_t *req, const char *name);

/* ---- body ---- */
int http_parse_body(const char *raw, int raw_len, http_request_t *req);

/* ---- one-shot full parse (request line + headers + body) ---- */
int http_parse_request(char *raw, int raw_len, http_request_t *req);

/* ---- MIME type from file extension ---- */
const char *http_content_type_from_path(const char *path);

/* ---- static file serving ---- */
int http_serve_file(const char *www_root, const char *uri,
                    char *buf, int buf_size, const char **content_type);

/* ---- response builder ---- */
int http_build_response(int status, const char *status_text,
                        const char *content_type,
                        const char *body, int body_len,
                        int keep_alive,
                        char *output, int size);

#endif /* HTTP_PARSER_H */
