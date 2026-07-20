#include "../include/config.h"
#include "../include/request_handler.h"
#include "../include/http_response.h"
#include "../include/http_parser.h"
#include "../include/log.h"
#include "../include/user_store.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- thread-local worker label (set by thread pool) ---- */
static __thread int g_worker_id = 0;

/* ---- v1.2.1: configurable document root for virtual hosting ---- */
static char g_current_root[128] = "www";

void request_handler_set_root(const char *root) {
    if (root && root[0] != '\0') {
        strncpy(g_current_root, root, sizeof(g_current_root) - 1);
        g_current_root[sizeof(g_current_root) - 1] = '\0';
    }
}

void request_handler_set_worker_label(int worker_id) {
    g_worker_id = worker_id;
}

int request_handler_get_worker_label_int(void) {
    return g_worker_id;
}

/*
 * Return a log prefix for the current worker thread.
 * Returns "[Worker-N] " when a label is set, or "" otherwise.
 */
static const char *worker_prefix(void) {
    static __thread char buf[32];
    if (g_worker_id > 0) {
        snprintf(buf, sizeof(buf), "[Worker-%d] ", g_worker_id);
        return buf;
    }
    return "";
}

int request_handler_parse(const char *line, request_t *req) {
    char method_buf[16];
    char path_buf[256];

    if (line == NULL || req == NULL) {
        return -1;
    }

    memset(req, 0, sizeof(*req));

    if (sscanf(line, "%15s %255s", method_buf, path_buf) != 2) {
        return -1;
    }

    strncpy(req->method, method_buf, sizeof(req->method) - 1);
    req->method[sizeof(req->method) - 1] = '\0';
    strncpy(req->path, path_buf, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    return 0;
}

void request_handler_set_body(const char *body, request_t *req) {
    if (body == NULL || req == NULL) {
        return;
    }
    strncpy(req->body, body, sizeof(req->body) - 1);
    req->body[sizeof(req->body) - 1] = '\0';
}

/*
 * Capture stdout from a void(*fn)(void) into a buffer.
 * Used for functions that printf directly (print_index, compare_search).
 */
static int capture_stdout(void (*fn)(void), char *buf, int size) {
    int pipefd[2];
    int saved_stdout;
    ssize_t n;

    if (pipe(pipefd) == -1) {
        return -1;
    }

    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* redirect stdout to the pipe */
    if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(saved_stdout);
        return -1;
    }
    close(pipefd[1]);

    /* call the printing function */
    fn();
    fflush(stdout);

    /* restore stdout */
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);

    /* read captured output */
    n = read(pipefd[0], buf, size - 1);
    close(pipefd[0]);

    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

/*
 * Wrapper struct + callback for compare_search_method with arguments.
 */
struct compare_ctx {
    const char *name;
    int verbose;
};

static void do_compare_search(void) {
    /* user_store_compare_search_method is called indirectly;
     * we need a way to pass arguments through a global or use a wrapper.
     * Since capture_stdout takes void(*)(void), use file-scope statics. */
}

/* file-scope context for compare_search wrapper */
static struct compare_ctx g_compare_ctx;

static void do_compare_search_wrapper(void) {
    user_store_compare_search_method(g_compare_ctx.name, g_compare_ctx.verbose);
}

static void do_print_index_wrapper(void) {
    user_store_print_index();
}

int request_handler_process(const request_t *req, char *output, int size) {
    int offset = 0;
    int written;

    if (req == NULL || output == NULL || size <= 0) {
        return -1;
    }

    /* ---- connection info header ---- */
    written = snprintf(output + offset, size - offset,
        "=== Connection Info ===\n"
        "Client IP: 127.0.0.1\n"
        "Client Port: 54321\n"
        "Server: MiniWeb\n"
        "=======================\n");
    if (written < 0 || written >= size - offset) {
        return -1;
    }
    offset += written;

    /* ================================================================
     *  route dispatching
     * ================================================================ */

    /* ---- GET /hello ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strcmp(req->path, "/hello") == 0) {
        char response[512];
        if (build_hello_response(response, sizeof(response)) < 0) {
            return -1;
        }
        written = snprintf(output + offset, size - offset, "%s", response);
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- GET /users (list all) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strcmp(req->path, "/users") == 0) {
        char cap[8192];
        if (capture_stdout(do_print_index_wrapper, cap, sizeof(cap)) < 0) {
            return -1;
        }
        written = snprintf(output + offset, size - offset, "%s", cap);
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- GET /users/find-index/<name> ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/find-index/", 18) == 0) {
        const char *name = req->path + 18;
        ListNode *user = user_store_find_index(name);
        if (user != NULL) {
            written = snprintf(output + offset, size - offset,
                "FOUND %s %s %s\n",
                user->data.name, user->data.password, user->data.mobile);
        } else {
            written = snprintf(output + offset, size - offset,
                "NOT_FOUND %s\n", name);
        }
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- GET /users/comppare-verbose/<name> (before /compare/ to match
     *      longer prefix first) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/compare-verbose/", 23) == 0) {
        const char *name = req->path + 23;
        char cap[8192];
        g_compare_ctx.name = name;
        g_compare_ctx.verbose = 1;
        if (capture_stdout(do_compare_search_wrapper, cap, sizeof(cap)) < 0) {
            return -1;
        }
        written = snprintf(output + offset, size - offset, "%s", cap);
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- GET /users/comppare/<name> ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/compare/", 15) == 0) {
        const char *name = req->path + 15;
        char cap[8192];
        g_compare_ctx.name = name;
        g_compare_ctx.verbose = 0;
        if (capture_stdout(do_compare_search_wrapper, cap, sizeof(cap)) < 0) {
            return -1;
        }
        written = snprintf(output + offset, size - offset, "%s", cap);
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- GET /user/<name> (simple find, must be after /users/ routes) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/user/", 6) == 0) {
        const char *name = req->path + 6;
        if (name[0] == '\0') {
            written = snprintf(output + offset, size - offset,
                "NOT_FOUND (empty name)\n");
        } else {
            ListNode *user = user_store_find(name);
            if (user != NULL) {
                written = snprintf(output + offset, size - offset,
                    "FOUND %s %s %s\n",
                    user->data.name, user->data.password, user->data.mobile);
            } else {
                written = snprintf(output + offset, size - offset,
                    "NOT_FOUND %s\n", name);
            }
        }
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- POST /users (add user, body = csv line) ---- */
    if (strcmp(req->method, "POST") == 0 &&
        strcmp(req->path, "/users") == 0) {
        int ret;
        if (req->body[0] == '\0') {
            written = snprintf(output + offset, size - offset,
                "ERROR: POST /users requires a body with CSV data\n");
            if (written < 0 || written >= size - offset) {
                return -1;
            }
            offset += written;
            return 0;
        }
        /* user_store_load_csv must already be called */
        ret = user_store_add(req->body);
        if (ret == 0) {
            written = snprintf(output + offset, size - offset, "ADDED\n");
        } else {
            written = snprintf(output + offset, size - offset, "EXISTS\n");
        }
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- DELETE /users/<name> ---- */
    if (strcmp(req->method, "DELETE") == 0 &&
        strncmp(req->path, "/users/", 7) == 0) {
        const char *name = req->path + 7;
        int ret;
        if (name[0] == '\0') {
            written = snprintf(output + offset, size - offset,
                "ERROR: DELETE requires a user name\n");
            if (written < 0 || written >= size - offset) {
                return -1;
            }
            offset += written;
            return 0;
        }
        ret = user_store_delete(name);
        if (ret == 0) {
            written = snprintf(output + offset, size - offset, "DELETED\n");
        } else {
            written = snprintf(output + offset, size - offset,
                "NO_SUCH_USER\n");
        }
        if (written < 0 || written >= size - offset) {
            return -1;
        }
        offset += written;
        return 0;
    }

    /* ---- fallback: 404 ---- */
    written = snprintf(output + offset, size - offset,
        "404 Not Found: %s %s\n", req->method, req->path);
    if (written < 0 || written >= size - offset) {
        return -1;
    }
    offset += written;
    return 0;
}

/* ================================================================
 *  HTTP response helpers
 * ================================================================ */

/*
 * Build a complete HTTP/1.1 response.
 * v1.1: keeps backward-compatible 5-arg signature.
 * For enhanced responses (dynamic Content-Type), use http_build_response() directly.
 */
static int build_http_response(char *output, int size,
                                int status, const char *status_text,
                                const char *body) {
    int written;
    int body_len;

    if (body == NULL) {
        body = "";
    }
    body_len = (int)strlen(body);

    written = snprintf(output, (size_t)size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: Keep-Alive\r\n"
        "\r\n"
        "%s",
        status, status_text, body_len, body);

    if (written < 0 || written >= size) {
        return -1;
    }
    return written;
}

/*
 * Build a user information string with all fields.
 */
static int format_user_info(const ListNode *user, char *buf, int size) {
    if (user == NULL || buf == NULL || size <= 0) {
        return -1;
    }
    return snprintf(buf, (size_t)size,
        "name: %s\n"
        "password: %s\n"
        "birthdate: %s\n"
        "phone: %s\n"
        "mobile: %s\n"
        "email: %s\n",
        user->data.name,
        user->data.password,
        user->data.birthdate,
        user->data.phone,
        user->data.mobile,
        user->data.email);
}

/* ================================================================
 *  request_handler_process_http
 *
 *  Same routing logic as request_handler_process, but outputs a
 *  proper HTTP/1.1 response with status codes.
 * ================================================================ */
/* ================================================================
 *  Static file response
 * ================================================================ */

/*
 * v1.2: Large static buffer for file serving (in .bss, not stack).
 * Supports images up to 5MB (logo 1.3MB, banner 640KB, avatar 4.1MB).
 */
#define BLOG_BODY_MAX (5 * 1024 * 1024)
static char g_body_buf[BLOG_BODY_MAX];

/*
 * v1.4: URL-decode a form-encoded value string.
 * Handles %XX hex sequences and '+' -> ' ' conversion.
 * Stops at '&' (parameter delimiter).
 * Returns 0 on success, -1 on invalid %XX sequence.
 */
static int url_decode(const char *src, char *dst, int dst_size) {
    int i = 0, j = 0;
    if (src == NULL || dst == NULL || dst_size <= 0) return -1;

    while (src[i] != '\0' && j < dst_size - 1) {
        if (src[i] == '%') {
            if (isxdigit((unsigned char)src[i + 1]) &&
                isxdigit((unsigned char)src[i + 2])) {
                char hex[3];
                hex[0] = src[i + 1];
                hex[1] = src[i + 2];
                hex[2] = '\0';
                dst[j++] = (char)strtol(hex, NULL, 16);
                i += 3;
            } else {
                return -1;  /* invalid %XX sequence */
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '&') {
            break;  /* next parameter */
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
    return 0;
}

int request_handler_process_http(const request_t *req, char *output, int size) {
    char *body = g_body_buf;
    int body_size = BLOG_BODY_MAX;
    int status;
    const char *status_text;

    if (req == NULL || output == NULL || size <= 0) {
        return -1;
    }

    memset(body, 0, body_size);

    /* ---- GET / (v1.2.1: serve from configurable root, default "www") ---- */
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/") == 0)) {
        const char *ct;
        int file_len = http_serve_file(g_current_root, "/", body, body_size, &ct);
        if (file_len > 0) {
            /* Use enhanced response builder with dynamic Content-Type */
            return http_build_response(200, "OK", ct, body, file_len,
                                     req->keep_alive > 0 ? req->keep_alive : 1,
                                     output, size);
        }
        /* file not found — fall through to 404 */
    }

    /* ================================================================
     *  GET /blog or /blog/* — serve blog static files (v1.2)
     *
     *  Status codes handled:
     *    301 — /blog without trailing slash → redirect to /blog/
     *    403 — path traversal attempt (.. in path)
     *    404 — file not found
     *    405 — method not allowed (non-GET/HEAD)
     * ================================================================ */
    if (strncmp(req->path, "/blog", 5) == 0) {

        /* ---- 405 Method Not Allowed ---- */
        if (strcmp(req->method, "GET") != 0 &&
            strcmp(req->method, "HEAD") != 0) {
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html><head><title>405 Method Not Allowed</title></head>\n"
                "<body><h1>405 Method Not Allowed</h1>\n"
                "<p>The method %s is not allowed for %s.</p>\n"
                "</body></html>\n",
                req->method, req->path);
            return http_build_response(405, "METHOD NOT ALLOWED",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       0, output, size);
        }

        /* ---- 403 Forbidden: path traversal check ---- */
        if (!http_is_safe_path(req->path)) {
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html><head><title>403 Forbidden</title></head>\n"
                "<body><h1>403 Forbidden</h1>\n"
                "<p>Access to this resource is forbidden.</p>\n"
                "</body></html>\n");
            return http_build_response(403, "FORBIDDEN",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       0, output, size);
        }

        /* ---- 301: /blog → /blog/ ---- */
        if (strcmp(req->path, "/blog") == 0) {
            return http_build_redirect(301, "/blog/", output, size);
        }

        /* ---- Serve static file from blog/ directory ---- */
        {
            const char *ct;
            int file_len;
            const char *blog_path;

            /*
             * /blog URL prefix always serves from the blog/ physical
             * directory (independent of virtual host root).  For
             * name-based virtual hosting, configure a server block
             * with "root blog;" and access it via the domain name.
             */
            /* req->path starts with "/blog" — strip the "/blog" prefix
             * to get the relative path within blog/ directory.
             * "/blog/"       → ""   → index.html
             * "/blog/css/style.css" → "/css/style.css"
             */
            blog_path = req->path + 5;  /* skip "/blog" */

            file_len = http_serve_file("blog", blog_path,
                                       body, body_size, &ct);
            if (file_len > 0) {
                /* ---- 304 Not Modified (if client sent If-Modified-Since) ---- */
                /* (v1.2: basic 304 support — compare file mtime with request header) */
                /*
                 * For full 304 support we would need access to the HTTP request
                 * headers.  As a minimal implementation, we always serve the file
                 * with a 200.  Future versions can add full caching support.
                 */

                return http_build_response(200, "OK", ct, body, file_len,
                                           req->keep_alive > 0 ? req->keep_alive : 1,
                                           output, size);
            }

            /* ---- 404 Not Found ---- */
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html><head><title>404 Not Found</title></head>\n"
                "<body><h1>404 Not Found</h1>\n"
                "<p>The requested URL %s was not found on this server.</p>\n"
                "<p><a href=\"/blog/\">Back to Blog Home</a></p>\n"
                "</body></html>\n",
                req->path);
            return http_build_response(404, "NOT FOUND",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       req->keep_alive > 0 ? req->keep_alive : 1,
                                       output, size);
        }
    }

    /* ---- v1.2.1: generic static file handler (CSS, JS, images, etc.) ---- */
    if (strcmp(req->method, "GET") == 0 ||
        strcmp(req->method, "HEAD") == 0) {
        if (!http_is_safe_path(req->path)) {
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html><head><title>403 Forbidden</title></head>\n"
                "<body><h1>403 Forbidden</h1></body></html>\n");
            return http_build_response(403, "FORBIDDEN",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       0, output, size);
        }

        {
            const char *ct;
            int file_len = http_serve_file(g_current_root, req->path,
                                           body, body_size, &ct);
            if (file_len > 0) {
                return http_build_response(200, "OK", ct, body, file_len,
                                           req->keep_alive > 0 ? req->keep_alive : 1,
                                           output, size);
            }
        }
    }

    /* ---- GET /hello ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strcmp(req->path, "/hello") == 0) {
        return build_http_response(output, size, 200, "OK",
                                   "Hello, Web!\n");
    }

    /* ---- GET /sleep/<ms> (for testing queue congestion) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/sleep/", 7) == 0) {
        int delay_ms = atoi(req->path + 7);
        if (delay_ms <= 0) {
            delay_ms = 100;
        }
        if (delay_ms > 5000) {
            delay_ms = 5000;  /* cap at 5 seconds */
        }

        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "%ssleeping %d ms", worker_prefix(), delay_ms);
            log_info(msg);
        }

        /* busy-wait or usleep — use usleep for simplicity */
        usleep((unsigned int)(delay_ms * 1000));

        {
            char body[64];
            snprintf(body, body_size, "Slept %d ms\n", delay_ms);
            return build_http_response(output, size, 200, "OK", body);
        }
    }

    /* ---- GET /help ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strcmp(req->path, "/help") == 0) {
        const char *help_text =
            "MiniWebServer v0.6 — HTTP API\n"
            "\n"
            "  GET  /hello                          hello\n"
            "  GET  /help                           this help\n"
            "  GET  /users                          list all users (BST inorder)\n"
            "  GET  /users/<name>                   find user by name\n"
            "  GET  /users/find-index/<name>        find user via BST index\n"
            "  GET  /users/compare/<name>           compare linked-list vs BST search\n"
            "  GET  /users/compare-verbose/<name>   compare search (verbose)\n"
            "  GET  /user/<name>                    find user by name\n"
            "  POST /users                          add user (body: csv line)\n"
            "  POST /search                        search users by partial name match\n"
            "  DELETE /users/<name>                 delete user\n";
        return build_http_response(output, size, 200, "OK", help_text);
    }

    /* ---- GET /users (list first 500, BST inorder — safe against pipe deadlock) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strcmp(req->path, "/users") == 0) {
        /*
         * v1.1 fix: capture_stdout() uses a pipe with limited kernel buffer (~64KB).
         * 100K users = ~4MB output → pipe fills up → fflush blocks → deadlock.
         * We generate the output directly into the body buffer with a safe limit.
         */
        int total = 0;
        int offset = 0;
        user_store_format_users(body, body_size - 1, &total, &offset);
        if (offset >= body_size - 200) {
            /* buffer near full — append truncation note */
            offset += snprintf(body + offset, body_size - offset,
                     "\n... (showing first entries out of %d total users)\n", total);
        }
        return build_http_response(output, size, 200, "OK", body);
    }

    /* ---- GET /users/find-index/<name> (before /users/<name> to
     *      match longer prefix first) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/find-index/", 18) == 0) {
        const char *name = req->path + 18;
        ListNode *user = user_store_find_index(name);
        if (user != NULL) {
            if (format_user_info(user, body, body_size) < 0) {
                return -1;
            }
            return build_http_response(output, size, 200, "OK", body);
        }
        snprintf(body, body_size, "NOT_FOUND %s\n", name);
        return build_http_response(output, size, 404, "NOT FOUND", body);
    }

    /* ---- GET /users/compare-verbose/<name> (before /compare/ and
     *      /users/<name> to match longer prefix first) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/compare-verbose/", 23) == 0) {
        const char *name = req->path + 23;
        g_compare_ctx.name = name;
        g_compare_ctx.verbose = 1;
        if (capture_stdout(do_compare_search_wrapper, body, body_size) < 0) {
            return build_http_response(output, size, 500,
                                       "INTERNAL SERVER ERROR",
                                       "500 Internal Server Error\n");
        }
        return build_http_response(output, size, 200, "OK", body);
    }

    /* ---- GET /users/compare/<name> (before /users/<name>) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/compare/", 15) == 0) {
        const char *name = req->path + 15;
        g_compare_ctx.name = name;
        g_compare_ctx.verbose = 0;
        if (capture_stdout(do_compare_search_wrapper, body, body_size) < 0) {
            return build_http_response(output, size, 500,
                                       "INTERNAL SERVER ERROR",
                                       "500 Internal Server Error\n");
        }
        return build_http_response(output, size, 200, "OK", body);
    }

    /* ---- DELETE /users/<name> (before GET /users/<name>) ---- */
    if (strcmp(req->method, "DELETE") == 0 &&
        strncmp(req->path, "/users/", 7) == 0) {
        const char *name = req->path + 7;
        int ret;
        if (name[0] == '\0') {
            return build_http_response(output, size, 400, "BAD REQUEST",
                                       "ERROR: DELETE requires a user name\n");
        }
        ret = user_store_delete(name);
        if (ret == 0) {
            return build_http_response(output, size, 200, "OK",
                                       "DELETED\n");
        }
        return build_http_response(output, size, 404, "NOT FOUND",
                                   "NO_SUCH_USER\n");
    }

    /* ---- GET /users/<name> (simple find by name) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/users/", 7) == 0) {
        const char *name = req->path + 7;
        if (name[0] == '\0') {
            return build_http_response(output, size, 400, "BAD REQUEST",
                                       "ERROR: empty user name\n");
        }
        {
            ListNode *user = user_store_find(name);
            if (user != NULL) {
                if (format_user_info(user, body, body_size) < 0) {
                    return -1;
                }
                return build_http_response(output, size, 200, "OK", body);
            }
            snprintf(body, body_size, "NOT_FOUND %s\n", name);
            return build_http_response(output, size, 404, "NOT FOUND", body);
        }
    }

    /* ---- GET /user/<name> (simple find, must be after /users/ routes) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        strncmp(req->path, "/user/", 6) == 0) {
        const char *name = req->path + 6;
        if (name[0] == '\0') {
            return build_http_response(output, size, 400, "BAD REQUEST",
                                       "ERROR: empty user name\n");
        }
        {
            ListNode *user = user_store_find(name);
            if (user != NULL) {
                if (format_user_info(user, body, body_size) < 0) {
                    return -1;
                }
                return build_http_response(output, size, 200, "OK", body);
            }
            snprintf(body, body_size, "NOT_FOUND %s\n", name);
            return build_http_response(output, size, 404, "NOT FOUND", body);
        }
    }

    /* ---- POST /users (add user, body = csv line) ---- */
    if (strcmp(req->method, "POST") == 0 &&
        strcmp(req->path, "/users") == 0) {
        int ret;
        if (req->body[0] == '\0') {
            return build_http_response(output, size, 400, "BAD REQUEST",
                "ERROR: POST /users requires a body with CSV data\n");
        }
        ret = user_store_add(req->body);
        if (ret == 0) {
            return build_http_response(output, size, 200, "OK", "ADDED\n");
        }
        return build_http_response(output, size, 200, "OK", "EXISTS\n");
    }

    /* ---- POST /search (v1.4: user search with form data) ---- */
    if (strcmp(req->method, "POST") == 0 &&
        strcmp(req->path, "/search") == 0) {

        /*
         * 1. Check Content-Type: only accept application/x-www-form-urlencoded
         */
        if (req->content_type[0] == '\0' ||
            strcasecmp(req->content_type,
                       "application/x-www-form-urlencoded") != 0) {
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html lang=\"en\"><head>\n"
                "<meta charset=\"utf-8\">\n"
                "<title>415 Unsupported Media Type</title>\n"
                "<style>"
                "body{font-family:sans-serif;max-width:640px;margin:48px auto;"
                "padding:0 16px;text-align:center;}"
                "h1{color:#dc2626;}"
                "</style></head><body>\n"
                "<h1>415 Unsupported Media Type</h1>\n"
                "<p>This endpoint only accepts "
                "<code>application/x-www-form-urlencoded</code>.</p>\n"
                "<p><a href=\"/\">Back to Search</a></p>\n"
                "</body></html>\n");
            return http_build_response(415, "UNSUPPORTED MEDIA TYPE",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       0, output, size);
        }

        /*
         * 2. Check Content-Length header is present
         */
        if (req->content_length_hdr < 0) {
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html lang=\"en\"><head>\n"
                "<meta charset=\"utf-8\">\n"
                "<title>400 Bad Request</title>\n"
                "<style>"
                "body{font-family:sans-serif;max-width:640px;margin:48px auto;"
                "padding:0 16px;text-align:center;}"
                "h1{color:#dc2626;}"
                "</style></head><body>\n"
                "<h1>400 Bad Request</h1>\n"
                "<p>Content-Length header is required.</p>\n"
                "<p><a href=\"/\">Back to Search</a></p>\n"
                "</body></html>\n");
            return http_build_response(400, "BAD REQUEST",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       0, output, size);
        }

        /*
         * 3. Check body length matches Content-Length
         */
        if ((int)strlen(req->body) != req->content_length_hdr) {
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html lang=\"en\"><head>\n"
                "<meta charset=\"utf-8\">\n"
                "<title>400 Bad Request</title>\n"
                "<style>"
                "body{font-family:sans-serif;max-width:640px;margin:48px auto;"
                "padding:0 16px;text-align:center;}"
                "h1{color:#dc2626;}"
                "</style></head><body>\n"
                "<h1>400 Bad Request</h1>\n"
                "<p>Body length (%d bytes) does not match "
                "Content-Length (%d bytes).</p>\n"
                "<p><a href=\"/\">Back to Search</a></p>\n"
                "</body></html>\n",
                (int)strlen(req->body), req->content_length_hdr);
            return http_build_response(400, "BAD REQUEST",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       0, output, size);
        }

        /*
         * 4. Extract and URL-decode form parameters (name, phone, email)
         */
        {
            search_criteria_t criteria;
            char name_buf[64], phone_buf[32], email_buf[64];
            char *p_ptr;
            int match_count;
            int has_criteria = 0;

            memset(&criteria, 0, sizeof(criteria));
            memset(name_buf, 0, sizeof(name_buf));
            memset(phone_buf, 0, sizeof(phone_buf));
            memset(email_buf, 0, sizeof(email_buf));

            /* parse "name=..." from body */
            p_ptr = strstr(req->body, "name=");
            if (p_ptr != NULL) {
                p_ptr += 5;
                if (url_decode(p_ptr, name_buf, sizeof(name_buf)) != 0) {
                    snprintf(body, body_size,
                        "<!DOCTYPE html>\n"
                        "<html lang=\"en\"><head>\n"
                        "<meta charset=\"utf-8\">\n"
                        "<title>400 Bad Request</title>\n"
                        "<style>"
                        "body{font-family:sans-serif;max-width:640px;"
                        "margin:48px auto;padding:0 16px;text-align:center;}"
                        "h1{color:#dc2626;}"
                        "</style></head><body>\n"
                        "<h1>400 Bad Request</h1>\n"
                        "<p>URL encoding error in name parameter.</p>\n"
                        "<p><a href=\"/\">Back to Search</a></p>\n"
                        "</body></html>\n");
                    return http_build_response(400, "BAD REQUEST",
                                               "text/html; charset=utf-8",
                                               body, (int)strlen(body),
                                               0, output, size);
                }
                if (name_buf[0] != '\0') {
                    strncpy(criteria.name, name_buf, sizeof(criteria.name) - 1);
                    has_criteria = 1;
                }
            }

            /* parse "phone=..." from body */
            p_ptr = strstr(req->body, "phone=");
            if (p_ptr != NULL) {
                p_ptr += 6;
                if (url_decode(p_ptr, phone_buf, sizeof(phone_buf)) != 0) {
                    snprintf(body, body_size,
                        "<!DOCTYPE html>\n"
                        "<html lang=\"en\"><head>\n"
                        "<meta charset=\"utf-8\">\n"
                        "<title>400 Bad Request</title>\n"
                        "<style>"
                        "body{font-family:sans-serif;max-width:640px;"
                        "margin:48px auto;padding:0 16px;text-align:center;}"
                        "h1{color:#dc2626;}"
                        "</style></head><body>\n"
                        "<h1>400 Bad Request</h1>\n"
                        "<p>URL encoding error in phone parameter.</p>\n"
                        "<p><a href=\"/\">Back to Search</a></p>\n"
                        "</body></html>\n");
                    return http_build_response(400, "BAD REQUEST",
                                               "text/html; charset=utf-8",
                                               body, (int)strlen(body),
                                               0, output, size);
                }
                if (phone_buf[0] != '\0') {
                    strncpy(criteria.phone, phone_buf, sizeof(criteria.phone) - 1);
                    has_criteria = 1;
                }
            }

            /* parse "email=..." from body */
            p_ptr = strstr(req->body, "email=");
            if (p_ptr != NULL) {
                p_ptr += 6;
                if (url_decode(p_ptr, email_buf, sizeof(email_buf)) != 0) {
                    snprintf(body, body_size,
                        "<!DOCTYPE html>\n"
                        "<html lang=\"en\"><head>\n"
                        "<meta charset=\"utf-8\">\n"
                        "<title>400 Bad Request</title>\n"
                        "<style>"
                        "body{font-family:sans-serif;max-width:640px;"
                        "margin:48px auto;padding:0 16px;text-align:center;}"
                        "h1{color:#dc2626;}"
                        "</style></head><body>\n"
                        "<h1>400 Bad Request</h1>\n"
                        "<p>URL encoding error in email parameter.</p>\n"
                        "<p><a href=\"/\">Back to Search</a></p>\n"
                        "</body></html>\n");
                    return http_build_response(400, "BAD REQUEST",
                                               "text/html; charset=utf-8",
                                               body, (int)strlen(body),
                                               0, output, size);
                }
                if (email_buf[0] != '\0') {
                    strncpy(criteria.email, email_buf, sizeof(criteria.email) - 1);
                    has_criteria = 1;
                }
            }

            /* 5. No criteria -> show form with prompt */
            if (!has_criteria) {
                snprintf(body, body_size,
                    "<!DOCTYPE html>\n"
                    "<html lang=\"zh-CN\"><head>\n"
                    "<meta charset=\"utf-8\">\n"
                    "<meta name=\"viewport\" content=\"width=device-width,"
                    "initial-scale=1\">\n"
                    "<title>User Search</title>\n"
                    "<link rel=\"icon\" href=\"/icon/favicon.ico\">\n"
                    "<style>"
                    ":root{--bg:#f5f5f5;--card-bg:#fff;--text:#333;"
                    "--muted:#666;--border:#e0e0e0;--accent:#2563eb;}"
                    "*{margin:0;padding:0;box-sizing:border-box;}"
                    "body{font-family:-apple-system,BlinkMacSystemFont,"
                    "\"Segoe UI\",Roboto,sans-serif;background:var(--bg);"
                    "color:var(--text);line-height:1.6;}"
                    "header{background:linear-gradient(135deg,#1e293b 0%%,"
                    "#334155 100%%);color:#fff;padding:40px 24px;"
                    "text-align:center;}"
                    "header h1{font-size:1.8rem;}"
                    "header p{margin-top:8px;opacity:0.8;}"
                    ".container{max-width:640px;margin:0 auto;"
                    "padding:32px 20px;}"
                    ".search-card{background:var(--card-bg);"
                    "border:1px solid var(--border);border-radius:12px;"
                    "padding:32px;box-shadow:0 2px 8px rgba(0,0,0,0.06);}"
                    ".search-card .field{margin-bottom:12px;text-align:left;}"
                    ".search-card label{display:block;font-size:0.85rem;"
                    "color:var(--muted);margin-bottom:4px;}"
                    ".search-card input[type=text]{padding:10px 16px;"
                    "font-size:1rem;width:100%%;border:1px solid #ccc;"
                    "border-radius:6px;outline:none;box-sizing:border-box;}"
                    ".search-card input[type=text]:focus{"
                    "border-color:var(--accent);}"
                    ".search-card .btn-row{text-align:center;margin-top:20px;}"
                    ".search-card button{padding:10px 32px;font-size:1rem;"
                    "background:var(--accent);color:#fff;border:none;"
                    "border-radius:6px;cursor:pointer;}"
                    ".search-card button:hover{opacity:0.9;}"
                    ".warning{color:#dc2626;margin-top:16px;text-align:center;"
                    "font-size:0.9rem;}"
                    ".hint{color:var(--muted);font-size:0.8rem;"
                    "text-align:center;margin-top:8px;}"
                    "footer{text-align:center;padding:24px 20px;"
                    "color:var(--muted);font-size:0.82rem;}"
                    "</style></head><body>\n"
                    "<header>\n"
                    "  <h1>&#128269; User Search</h1>\n"
                    "  <p>100K Chinese Users &middot; RBT Index &middot; AND Search</p>\n"
                    "</header>\n"
                    "<div class=\"container\">\n"
                    "<div class=\"search-card\">\n"
                    "<form method=\"post\" action=\"/search\""
                    " enctype=\"application/x-www-form-urlencoded\">\n"
                    "<div class=\"field\">"
                    "<label>Name</label>"
                    "<input type=\"text\" name=\"name\""
                    " placeholder=\"e.g. 赵安\" autofocus>"
                    "</div>\n"
                    "<div class=\"field\">"
                    "<label>Phone</label>"
                    "<input type=\"text\" name=\"phone\""
                    " placeholder=\"e.g. 138\">"
                    "</div>\n"
                    "<div class=\"field\">"
                    "<label>Email</label>"
                    "<input type=\"text\" name=\"email\""
                    " placeholder=\"e.g. @example.com\">"
                    "</div>\n"
                    "<div class=\"btn-row\">"
                    "<button type=\"submit\">Search</button>"
                    "</div>\n"
                    "</form>\n"
                    "<p class=\"hint\">All fields are combined with AND &middot; "
                    "Case-insensitive &middot; Partial match &middot; UTF-8</p>\n"
                    "<p class=\"warning\">"
                    "Please enter at least one search criterion.</p>\n"
                    "</div>\n</div>\n"
                    "<footer>MiniWeb Server v1.4</footer>\n"
                    "</body></html>\n");
                return http_build_response(200, "OK",
                                           "text/html; charset=utf-8",
                                           body, (int)strlen(body),
                                           req->keep_alive > 0 ? req->keep_alive : 1,
                                           output, size);
            }

            /* 6. Multi-criteria AND search via RBT */
            match_count = user_store_search(&criteria, body, body_size);
            if (match_count < 0) {
                return http_build_response(500, "INTERNAL SERVER ERROR",
                                           "text/plain; charset=utf-8",
                                           "500 Internal Server Error\n", 25,
                                           0, output, size);
            }

            return http_build_response(200, "OK",
                                       "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       req->keep_alive > 0 ? req->keep_alive : 1,
                                       output, size);
        }
    }

    /* ---- fallback: 404 ---- */
    snprintf(body, body_size, "404 Not Found: %s %s\n",
             req->method, req->path);
    return build_http_response(output, size, 404, "NOT FOUND", body);
}

/* ================================================================
 *  Connection-level helpers (shared by tcp_server and tcp_fork_server)
 * ================================================================ */

#define RECV_BUF_SIZE 8192
#define RESP_BUF_SIZE (5 * 1024 * 1024 + 4096)  /* v1.2: 5MB + headers for images */

/*
 * Parse the first line of an HTTP request into method and path.
 * Expects: "METHOD /path HTTP/1.x"
 * Modifies line in-place (null-terminates method and path).
 * Returns 0 on success, -1 on failure.
 */
static int parse_http_request_line(char *line, char **method, char **path) {
    char *p;

    *method = line;

    p = strchr(line, ' ');
    if (p == NULL) {
        return -1;
    }
    *p = '\0';
    p++;

    while (*p == ' ') {
        p++;
    }

    *path = p;

    p = strchr(p, ' ');
    if (p != NULL) {
        *p = '\0';
    }

    return 0;
}

/*
 * Extract the request body from the raw HTTP request.
 * Looks for \r\n\r\n to find the end of headers; everything after
 * that is the body.  Returns a pointer into raw (or NULL if no body).
 */
static char *extract_body(char *raw, int len) {
    char *body_start;

    body_start = strstr(raw, "\r\n\r\n");
    if (body_start == NULL) {
        return NULL;
    }

    body_start += 4;

    if (body_start >= raw + len) {
        return NULL;
    }

    {
        char *end = body_start + strlen(body_start);
        while (end > body_start &&
               (*(end - 1) == '\r' || *(end - 1) == '\n')) {
            end--;
            *end = '\0';
        }
    }

    if (body_start[0] == '\0') {
        return NULL;
    }

    return body_start;
}

int request_handler_handle_connection(int conn_fd) {
    char msg[512];
    int request_count = 0;
    struct timeval tv;
    ssize_t n;

    /* ---- set keep-alive idle timeout ---- */
    tv.tv_sec = KEEP_ALIVE_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ================================================================
     *  keep-alive loop — handle multiple requests per connection
     * ================================================================ */
    while (request_count < MAX_KEEP_ALIVE_REQUESTS) {
        char recv_buf[RECV_BUF_SIZE];
        static char resp_buf[RESP_BUF_SIZE];  /* v1.2: .bss to avoid stack overflow */

        /* ---- read HTTP request ---- */
        n = recv(conn_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n <= 0) {
            if (n == 0) {
                /* client closed connection cleanly */
                snprintf(msg, sizeof(msg),
                         "%sclient closed connection (after %d request(s))",
                         worker_prefix(), request_count);
                log_info(msg);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* idle timeout between requests */
                snprintf(msg, sizeof(msg),
                         "%skeep-alive idle timeout after %d request(s)",
                         worker_prefix(), request_count);
                log_info(msg);
            } else {
                char _msg[128];
                snprintf(_msg, sizeof(_msg), "%srecv() failed on request #%d",
                         worker_prefix(), request_count + 1);
                log_error(_msg);
                return -1;
            }
            break;
        }
        recv_buf[n] = '\0';

        /* ---- parse the first line ---- */
        {
            char *method;
            char *path;
            char line_copy[512];
            request_t req;
            char *body;
            char *nl;
            int resp_status;

            strncpy(line_copy, recv_buf, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';
            nl = strchr(line_copy, '\r');
            if (nl != NULL) *nl = '\0';
            nl = strchr(line_copy, '\n');
            if (nl != NULL) *nl = '\0';

            if (parse_http_request_line(line_copy, &method, &path) != 0) {
                snprintf(msg, sizeof(msg), "%smalformed HTTP request line",
                         worker_prefix());
                log_error(msg);
                snprintf(resp_buf, sizeof(resp_buf),
                         "HTTP/1.1 400 BAD REQUEST\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Length: 17\r\n"
                         "Connection: close\r\n"
                         "\r\n"
                         "400 Bad Request\n");
                send(conn_fd, resp_buf, strlen(resp_buf), 0);
                snprintf(msg, sizeof(msg),
                         "%sresponse: (malformed) -> 400 BAD REQUEST",
                         worker_prefix());
                log_info(msg);
                return -1;
            }

            snprintf(msg, sizeof(msg),
                     "%srequest: %s %s", worker_prefix(), method, path);
            log_info(msg);

            /* fill request_t — normalize method to uppercase */
            memset(&req, 0, sizeof(req));
            req.content_length_hdr = -1;  /* v1.4: -1 = absent */
            {
                int i;
                for (i = 0; method[i] != '\0' && i < (int)sizeof(req.method) - 1; i++) {
                    req.method[i] = (char)toupper((unsigned char)method[i]);
                }
                req.method[i] = '\0';
            }
            strncpy(req.path, path, sizeof(req.path) - 1);
            req.path[sizeof(req.path) - 1] = '\0';

            /* extract body */
            body = extract_body(recv_buf, (int)n);
            if (body != NULL) {
                request_handler_set_body(body, &req);
                snprintf(msg, sizeof(msg),
                         "%sbody: %s", worker_prefix(), body);
                log_info(msg);
            }

            /* ---- v1.4: extract Content-Type and Content-Length headers ---- */
            {
                const char *ct_start = NULL;
                const char *cl_start = NULL;
                {
                    char *p = recv_buf;
                    int limit = (int)n;
                    while (p < recv_buf + limit - 14) {
                        if ((p[0] == '\n' || p[0] == '\r') &&
                            (p[1] == 'C' || p[1] == 'c') &&
                            (p[2] == 'o' || p[2] == 'O') &&
                            (p[3] == 'n' || p[3] == 'N')) {
                            if ((p[4] == 't' || p[4] == 'T') &&
                                (p[5] == 'e' || p[5] == 'E') &&
                                (p[6] == 'n' || p[6] == 'N') &&
                                (p[7] == 't' || p[7] == 'T') &&
                                (p[8] == '-')) {
                                if ((p[9] == 'T' || p[9] == 't') &&
                                    (p[10] == 'y' || p[10] == 'Y') &&
                                    (p[11] == 'p' || p[11] == 'P') &&
                                    (p[12] == 'e' || p[12] == 'E') &&
                                    p[13] == ':') {
                                    ct_start = p + 14;
                                } else if ((p[9] == 'L' || p[9] == 'l') &&
                                           (p[10] == 'e' || p[10] == 'E') &&
                                           (p[11] == 'n' || p[11] == 'N') &&
                                           (p[12] == 'g' || p[12] == 'G') &&
                                           (p[13] == 't' || p[13] == 'T') &&
                                           p[14] == 'h' && p[15] == ':') {
                                    cl_start = p + 16;
                                }
                            }
                        }
                        p++;
                    }
                }

                if (ct_start) {
                    int k = 0;
                    while (*ct_start == ' ' || *ct_start == '\t') ct_start++;
                    while (ct_start[k] != '\0' &&
                           ct_start[k] != '\r' && ct_start[k] != '\n' &&
                           k < (int)sizeof(req.content_type) - 1) {
                        req.content_type[k] = ct_start[k];
                        k++;
                    }
                    req.content_type[k] = '\0';
                    /* strip trailing whitespace */
                    {
                        char *e = req.content_type + strlen(req.content_type) - 1;
                        while (e >= req.content_type &&
                               (*e == ' ' || *e == '\t')) {
                            *e = '\0'; e--;
                        }
                    }
                }

                if (cl_start) {
                    while (*cl_start == ' ' || *cl_start == '\t') cl_start++;
                    req.content_length_hdr = atoi(cl_start);
                    if (req.content_length_hdr < 0) req.content_length_hdr = 0;
                }
            }

            /* ---- v1.2.1: extract Host header for virtual host routing ---- */
            {
                const char *host_start;
                /*
                 * Scan recv_buf for "\nHost:" or "\nhost:" (case-insensitive).
                 * The Host header typically appears early in the request,
                 * so a simple strcasestr-like scan of the first 2KB is
                 * sufficient.
                 */
                host_start = NULL;
                {
                    char *p = recv_buf;
                    while (p < recv_buf + n - 6) {
                        if ((p[0] == '\n' || p[0] == '\r') &&
                            (p[1] == 'H' || p[1] == 'h') &&
                            (p[2] == 'O' || p[2] == 'o') &&
                            (p[3] == 'S' || p[3] == 's') &&
                            (p[4] == 'T' || p[4] == 't') &&
                            p[5] == ':') {
                            host_start = p + 6;
                            break;
                        }
                        p++;
                    }
                }

                if (host_start) {
                    char host_buf[256];
                    char *end;
                    int k = 0;

                    /* skip leading whitespace */
                    while (*host_start == ' ' || *host_start == '\t')
                        host_start++;

                    /* copy value until \r or \n */
                    while (host_start[k] != '\0' &&
                           host_start[k] != '\r' &&
                           host_start[k] != '\n' &&
                           k < (int)sizeof(host_buf) - 1) {
                        host_buf[k] = host_start[k];
                        k++;
                    }
                    host_buf[k] = '\0';

                    /* strip trailing whitespace */
                    end = host_buf + strlen(host_buf) - 1;
                    while (end >= host_buf &&
                           (*end == ' ' || *end == '\t')) {
                        *end = '\0';
                        end--;
                    }

                    /* strip port suffix */
                    {
                        char *colon = strchr(host_buf, ':');
                        if (colon) *colon = '\0';
                    }

                    /* match against server blocks */
                    if (host_buf[0] != '\0') {
                        const server_block_t *sb =
                            config_find_server(NULL, host_buf);
                        if (sb && sb->root[0] != '\0') {
                            request_handler_set_root(sb->root);
                        }
                    }
                }
            }

            /* generate HTTP response */
            {
                int resp_len = request_handler_process_http(&req, resp_buf,
                                              sizeof(resp_buf));
                if (resp_len < 0) {
                    snprintf(msg, sizeof(msg),
                             "%srequest_handler_process_http failed",
                             worker_prefix());
                    log_error(msg);
                    resp_len = snprintf(resp_buf, sizeof(resp_buf),
                             "HTTP/1.1 500 INTERNAL SERVER ERROR\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 25\r\n"
                             "Connection: close\r\n"
                             "\r\n"
                             "500 Internal Server Error\n");
                }

                /* extract and log response status */
                if (strncmp(resp_buf, "HTTP/1.1 ", 9) == 0) {
                    resp_status = atoi(resp_buf + 9);
                    snprintf(msg, sizeof(msg),
                             "%sresponse: %s %s -> %d",
                             worker_prefix(), method, path, resp_status);
                } else {
                    resp_status = 0;
                    snprintf(msg, sizeof(msg),
                             "%sresponse: %s %s -> (unknown status)",
                             worker_prefix(), method, path);
                }
                log_info(msg);

                /* console output */
                printf("%s %s -> %d\n", method, path, resp_status);

                /* ---- send response (v1.2: use actual length for binary) ---- */
                {
                    ssize_t total = 0;
                    ssize_t remaining = (ssize_t)resp_len;
                    const char *ptr = resp_buf;

                while (remaining > 0) {
                    ssize_t sent = send(conn_fd, ptr, (size_t)remaining, 0);
                    if (sent < 0) {
                        snprintf(msg, sizeof(msg), "%ssend() failed (errno=%d)",
                                 worker_prefix(), errno);
                        log_error(msg);
                        return -1;
                    }
                    total += sent;
                    ptr += sent;
                    remaining -= sent;
                }
            }  /* end send response block */
            }  /* end generate HTTP response block (v1.2 resp_len scope) */
        }

        request_count++;
    }

    return (request_count > 0) ? 0 : -1;
}
