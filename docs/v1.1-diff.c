/* ================================================================
 * mini_web_server v1.0 → v1.1 代码差异汇总
 *
 * 标记规则:
 *   [NEW]      — v1.1 新增文件，完整代码
 *   [MODIFIED] — v1.1 修改文件，只包含变更部分
 *   ... 省略   — 未变更代码用此标记
 *
 * 忽略: README.md, docs/debug-log.txt, www/index.html,
 *       tests/bench_keepalive.sh 等非 C 代码文件
 * ================================================================ */

/* ================================================================
 * [NEW] include/http_parser.h
 * HTTP 请求解析器 + 响应构建器 + 静态文件服务 API
 * ================================================================ */
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

/* ================================================================
 * [NEW] src/http_parser.c
 * 完整实现: 请求行/头部/body 解析、MIME 映射、静态文件读取、响应构建
 * ================================================================ */
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
        "Server: MiniWeb/1.1\r\n"
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
 * [MODIFIED] include/request_handler.h
 * request_t 增加 HTTP 版本和 keep-alive 字段
 * ================================================================ */

// v1.0 原始:
// typedef struct {
//     char method[16];
//     char path[256];
//     char body[512];   /* CSV data for POST /users (addusr) */
// } request_t;

// v1.1 修改为:
typedef struct {
    char method[16];
    char path[256];
    char body[512];   /* CSV data for POST /users (addusr) */

    /* v1.1: HTTP version and keep-alive from header parsing */
    int  http_version_major;   /* 1 */
    int  http_version_minor;   /* 0 or 1 */
    int  keep_alive;           /* 1=keep connection, 0=close */
} request_t;

// v1.0 原始:
// #define MAX_KEEP_ALIVE_REQUESTS  100

// v1.1 修改为 (ab -k -n 10000 压测需要更高的上限):
#define MAX_KEEP_ALIVE_REQUESTS  10000   /* max requests per connection (v1.1: raised for ab benchmarks) */

/* ================================================================
 * [MODIFIED] include/epoll_server.h
 * epoll_server_worker_run 增加 worker_id 参数
 * ================================================================ */

// v1.0 原始:
// int epoll_server_worker_run(int listen_fd);

// v1.1 修改为:
int epoll_server_worker_run(int listen_fd, int worker_id);

/* ================================================================
 * [MODIFIED] include/user_index.h
 * 新增 bst_format_users 声明
 * ================================================================ */

// 在 void bst_free(BST *tree); 之后新增:
/*
 * v1.1: inorder traversal that writes to a buffer.
 * Stops when buf is full. *total is user count, *offset is bytes written.
 */
void bst_format_users(BST *tree, char *buf, int buf_size, int *total, int *offset);

/* ================================================================
 * [MODIFIED] include/user_store.h
 * 新增 user_store_format_users 声明
 * ================================================================ */

// 在 void user_store_print_index(void); 之后新增:
/* v1.1: format users into a buffer (BST inorder), safe for large datasets. */
void user_store_format_users(char *buf, int buf_size, int *total, int *offset);

/* ================================================================
 * [MODIFIED] src/request_handler.c
 * ================================================================ */

// ---- 新增 include ----
#include "../include/http_parser.h"

// ---- body 缓冲区从 8KB → 16KB (index.html 9937 字节) ----
// v1.0:  char body[8192];
// v1.1:  char body[16384];

// ---- 新增 GET / 路由 (在 memset(body, 0, sizeof(body)) 之后) ----
/*
    // v1.1: serve www/index.html with text/html Content-Type
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/") == 0)) {
        const char *ct;
        int file_len = http_serve_file("www", "/", body, sizeof(body), &ct);
        if (file_len > 0) {
            if (http_build_response(200, "OK", ct, body, file_len,
                                     req->keep_alive > 0 ? req->keep_alive : 1,
                                     output, size) < 0)
                return -1;
            return 0;
        }
        // file not found — fall through to 404
    }
*/

// ---- GET /users 路由: capture_stdout → 直接 buffer 写入 (修复 pipe 死锁) ----
// v1.0:
//   capture_stdout(do_print_index_wrapper, ...) → pipe 死锁 (100K 用户 ~4MB > 64KB 管道缓冲)
// v1.1:
/*
    int total = 0, offset = 0;
    user_store_format_users(body, sizeof(body) - 1, &total, &offset);
    if (offset >= sizeof(body) - 200) {
        offset += snprintf(body + offset, sizeof(body) - offset,
                 "\n... (showing first entries out of %d total users)\n", total);
    }
    return build_http_response(output, size, 200, "OK", body);
*/

// ---- Connection 头大小写修正 (AB 2.3 大小写敏感) ----
// v1.0:  "Connection: keep-alive\r\n"  (全小写, AB 不认)
// v1.1:  "Connection: Keep-Alive\r\n"  (首字母大写)

/* ================================================================
 * [MODIFIED] src/epoll_server.c
 * ================================================================ */

// ---- 新增 include ----
#include "../include/http_parser.h"

// ---- Worker ID 跟踪 ----
static int g_worker_id = 0;  // v1.1: worker number (1-based)

// ---- safe_close: 优雅关闭避免 RST ----
/*
 * v1.1: graceful close to avoid RST on keep-alive connections.
 * shutdown(SHUT_RDWR) → drain pending data → close.
 * 替代所有裸 close(ready_fd) 和 close(connections[i].fd) 调用。
 */
static void safe_close(int fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        char dummy[256];
        while (recv(fd, dummy, sizeof(dummy), 0) > 0) {}  // drain
        close(fd);
    }
}

// ---- 启动日志: 显示 Worker-N ----
// v1.0: "Worker PID: %d"
// v1.1: "Worker-%d PID: %d", g_worker_id, (int)getpid()

// ---- Keep-Alive 协商: HAS_KEEPALIVE / HAS_CLOSE 宏 ----
/*
 * v1.1: 检查 4 种大小写组合, 兼容 AB (HTTP/1.0 + Connection: Keep-Alive)
 *       和 curl (Connection: keep-alive)
 */
#define HAS_KEEPALIVE(haystack) \
    (strstr(haystack, "Connection: keep-alive") || \
     strstr(haystack, "Connection: Keep-Alive") || \
     strstr(haystack, "connection: keep-alive") || \
     strstr(haystack, "connection: Keep-Alive"))
#define HAS_CLOSE(haystack) \
    (strstr(haystack, "Connection: close") || \
     strstr(haystack, "Connection: Close") || \
     strstr(haystack, "connection: close") || \
     strstr(haystack, "connection: Close"))

// 原有: 只检查 2 种全小写变体 → 漏掉 AB 的 Keep-Alive (大写)
// 修改: 检查 4 种大小写组合

// ---- 请求日志: Worker-N 前缀 + fd/path/status ----
// v1.0: "[EpollServer] fd=%d %s %s -> %d", ready_fd, method, path, resp_status
// v1.1:
/*
    if (g_worker_mode) {
        snprintf(msg, sizeof(msg),
                 "[Worker-%d] fd=%d %s %s -> %d",
                 g_worker_id, ready_fd, method, path, resp_status);
    } else {
        snprintf(msg, sizeof(msg),
                 "[EpollServer] fd=%d %s %s -> %d",
                 ready_fd, method, path, resp_status);
    }
*/

// ---- Connection 响应头修正 (req.keep_alive=0 时替换为 close) ----
/*
    if (!req.keep_alive) {
        char *ka = strstr(resp_buf, "Connection: Keep-Alive");
        if (ka) {
            memcpy(ka + 12, "close     ", 10);  // "Keep-Alive"(10) → "close"(5+5空格)
        }
    }
*/

// ---- Keep-Alive 关闭条件 ----
// v1.0:  if (conn->request_count >= MAX_KEEP_ALIVE_REQUESTS)
// v1.1:  if (conn->request_count >= MAX_KEEP_ALIVE_REQUESTS || !req.keep_alive)
//        增加 HTTP 协议层面的关闭: HTTP/1.0 或 Connection: close → req.keep_alive=0 → 关闭

// ---- epoll_server_worker_run 增加 worker_id ----
/*
int epoll_server_worker_run(int listen_fd, int worker_id) {
    g_worker_mode       = 1;
    g_worker_id         = worker_id;
    g_master_listen_fd  = listen_fd;
    g_shutdown          = 0;
    g_worker_shutdown   = 0;
    return epoll_server_run("0.0.0.0", 0);
}
*/

/* ================================================================
 * [MODIFIED] src/master_worker.c
 * 传递 worker_id 给 epoll_server_worker_run
 * ================================================================ */

// v1.0:  epoll_server_worker_run(listen_fd);
// v1.1:  epoll_server_worker_run(listen_fd, i + 1);  /* worker IDs are 1-based */

/* ================================================================
 * [MODIFIED] src/user_index.c
 * 新增 bst_format_users — 安全地向 buffer 写用户数据 (避免 pipe 死锁)
 * ================================================================ */

// 在 bst_inorder() 之后新增:
typedef struct {
    char *buf;
    int   buf_size;
    int  *total;
    int  *offset;
} bst_format_ctx;

static void bst_format_node(BSTnode *node, BSTnode *nil, bst_format_ctx *ctx) {
    char line[256];
    int n;
    if (node == nil) return;
    if (!ctx || !ctx->buf || ctx->buf_size <= 0) return;
    bst_format_node(node->left, nil, ctx);
    if (*(ctx->offset) >= ctx->buf_size - 256) return;  // buffer near full
    (*(ctx->total))++;
    n = snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%s\n",
                 node->user->data.name, node->user->data.password,
                 node->user->data.birthdate, node->user->data.phone,
                 node->user->data.mobile, node->user->data.email);
    if (*(ctx->offset) + n < ctx->buf_size) {
        memcpy(ctx->buf + *(ctx->offset), line, n);
        *(ctx->offset) += n;
    }
    bst_format_node(node->right, nil, ctx);
}

void bst_format_users(BST *tree, char *buf, int buf_size, int *total, int *offset) {
    bst_format_ctx ctx;
    if (!tree || !buf || buf_size <= 0) return;
    *total  = 0;
    *offset = 0;
    ctx.buf = buf; ctx.buf_size = buf_size;
    ctx.total = total; ctx.offset = offset;
    bst_format_node(tree->root, &tree->nil, &ctx);
}

/* ================================================================
 * [MODIFIED] src/user_store.c
 * 新增 user_store_format_users 包装函数
 * ================================================================ */

// 在 user_store_print_index() 之后新增:
void user_store_format_users(char *buf, int buf_size, int *total, int *offset) {
    if (!buf || buf_size <= 0 || !total || !offset) return;
    bst_format_users(&user_bst, buf, buf_size, total, offset);
}

/* ================================================================
 * [MODIFIED] Makefile
 * ================================================================ */

// ---- mini_web_server 链接目标增加 obj/http_parser.o ----
// v1.0:
//   mini_web_server: ... obj/master_worker.o
// v1.1:
//   mini_web_server: ... obj/master_worker.o obj/http_parser.o

// ---- 新增 http_parser.o 编译规则 ----
// obj/http_parser.o: src/http_parser.c include/http_parser.h include/log.h
//         gcc -g -I./include -c src/http_parser.c -o obj/http_parser.o

// ---- EpollServer, request_worker 链接目标同样增加 obj/http_parser.o ----

/* ================================================================
 * 变更汇总
 * ================================================================
 *
 * 新增文件:
 *   include/http_parser.h  — HTTP 解析器 + 响应构建器 API (83 行)
 *   src/http_parser.c      — 完整实现 (434 行)
 *
 * 修改文件:
 *   include/request_handler.h — request_t 扩展 + MAX_KEEP_ALIVE_REQUESTS 调整
 *   include/epoll_server.h    — worker_run 签名增加 worker_id
 *   include/user_index.h      — bst_format_users 声明
 *   include/user_store.h      — user_store_format_users 声明
 *   src/request_handler.c     — GET /, /users 修复, Connection 头大小写
 *   src/epoll_server.c        — keep-alive 协商, safe_close, Worker-N 日志
 *   src/master_worker.c       — 传递 worker_id
 *   src/user_index.c          — bst_format_users 实现 (58 行)
 *   src/user_store.c          — user_store_format_users 包装
 *   Makefile                  — http_parser.o 编译链接
 *
 * 修复的 Bug:
 *   1. AB Keep-Alive requests: 0 → Connection 头大小写 (3 轮调试)
 *   2. /users 端点 pipe 死锁 → 直接 buffer 写入
 *   3. Connection reset by peer → safe_close (shutdown + drain + close)
 *   4. 日志无法区分 worker → [Worker-N] 前缀
 *
 * 性能提升 (ab -k -c 50 -n 10000 /hello):
 *   Keep-Alive ON:  2,543 req/s  (v1.1)
 *   Keep-Alive OFF:   764 req/s  (基准)
 *   提升: 3.3x
 * ================================================================ */
