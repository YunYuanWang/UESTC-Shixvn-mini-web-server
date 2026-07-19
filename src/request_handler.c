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

int request_handler_process_http(const request_t *req, char *output, int size) {
    char *body = g_body_buf;
    int body_size = BLOG_BODY_MAX;
    int status;
    const char *status_text;

    if (req == NULL || output == NULL || size <= 0) {
        return -1;
    }

    memset(body, 0, body_size);

    /* ---- GET / (v1.1: serve www/index.html with text/html Content-Type) ---- */
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/") == 0)) {
        const char *ct;
        int file_len = http_serve_file("www", "/", body, body_size, &ct);
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
