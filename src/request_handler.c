#include "../include/config.h"
#include "../include/request_handler.h"
#include "../include/route_table.h"
#include "../include/session.h"
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* v1.4: forward declaration for URL path decoding */
static char *url_decode_path(char *src);

/* v1.6/v1.7: forward declarations for auth */
static const char *auth_lookup(const char *username, const char *password);
static int auth_role_matches(const char *user_role, const char *required_role);

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
        url_decode_path((char *)name);
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
        url_decode_path((char *)name);
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
        url_decode_path((char *)name);
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
        url_decode_path((char *)name);
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
        url_decode_path((char *)name);
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
        "Content-Type: text/plain; charset=utf-8\r\n"
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

/*
 * v1.4: URL-decode a path segment (only %XX, no + → space).
 * Decodes in-place. Returns the decoded string (same pointer).
 * Invalid %XX sequences are left as-is.
 */
static char *url_decode_path(char *src) {
    char *p, *q;
    if (src == NULL) return NULL;

    p = src;
    q = src;
    while (*p != '\0') {
        if (*p == '%' &&
            isxdigit((unsigned char)p[1]) &&
            isxdigit((unsigned char)p[2])) {
            char hex[3];
            hex[0] = p[1];
            hex[1] = p[2];
            hex[2] = '\0';
            *q++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
    return src;
}

/*
 * v1.4: Validate phone — digits and hyphens only, 1-20 chars, >= 1 digit.
 * Empty string is valid (means "no filter").
 */
static int validate_phone(const char *phone) {
    int digits = 0, len = 0;
    if (phone == NULL || phone[0] == '\0') return 1;
    for (const char *p = phone; *p != '\0'; p++, len++) {
        if (*p >= '0' && *p <= '9') digits++;
        else if (*p != '-') return 0;
    }
    return (len >= 1 && len <= 20 && digits >= 1);
}

/*
 * v1.4: Validate email — must have exactly one '@', local and domain parts,
 * domain must contain '.'.  Empty string is valid (means "no filter").
 */
static int validate_email(const char *email) {
    const char *at;
    if (email == NULL || email[0] == '\0') return 1;
    /* Must not contain spaces */
    if (strchr(email, ' ') != NULL) return 0;
    /* If no '@', accept as partial search term (e.g. domain search) */
    at = strchr(email, '@');
    if (at == NULL) return 1;
    /* Has '@': must have at most one, with something after it */
    if (strchr(at + 1, '@') != NULL) return 0;   /* multiple @ */
    if (at[1] == '\0') return 0;                   /* nothing after @ */
    /* '@' at start is OK for domain search like @example.com */
    return 1;
}

/* ================================================================
 *  v1.5: Route-table-based handler functions
 *
 *  Each handler encapsulates one route's logic.  They are called by
 *  request_handler_process_http() via the route table dispatch.
 *  All handlers share the signature defined in request_handler.h.
 * ================================================================ */

/* ---- API handlers ---- */

static int handle_hello(const request_t *req, char *body, int body_size,
                         const char *captured, char *output, int output_size) {
    (void)req; (void)body; (void)body_size; (void)captured;
    return build_http_response(output, output_size, 200, "OK",
                               "Hello, Web!\n");
}

static int handle_help(const request_t *req, char *body, int body_size,
                        const char *captured, char *output, int output_size) {
    (void)req; (void)body; (void)body_size; (void)captured;
    const char *help_text =
        "MiniWebServer v1.5 — HTTP API\n"
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
        "  POST /search                         search users by partial name match\n"
        "  DELETE /users/<name>                 delete user\n";
    return build_http_response(output, output_size, 200, "OK", help_text);
}

static int handle_sleep(const request_t *req, char *body, int body_size,
                         const char *captured, char *output, int output_size) {
    (void)req;
    int delay_ms = atoi(captured);
    if (delay_ms <= 0) delay_ms = 100;
    if (delay_ms > 5000) delay_ms = 5000;

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "%ssleeping %d ms", worker_prefix(), delay_ms);
        log_info(msg);
    }

    usleep((unsigned int)(delay_ms * 1000));
    snprintf(body, body_size, "Slept %d ms\n", delay_ms);
    return build_http_response(output, output_size, 200, "OK", body);
}

/* ---- Index handler ---- */

static int handle_index(const request_t *req, char *body, int body_size,
                         const char *captured, char *output, int output_size) {
    (void)captured;

    /* v1.7: Get username from session */
    const char *username = NULL;
    if (req->cookie[0] != '\0') {
        const char *sid_start = strstr(req->cookie, "session_id=");
        if (sid_start) {
            sid_start += 11; char sid[SESSION_ID_LEN]; int si = 0;
            while (sid_start[si] && sid_start[si] != ';' && sid_start[si] != ' '
                   && si < (int)sizeof(sid) - 1) sid[si++] = sid_start[si];
            sid[si] = '\0';
            session_t *s = session_lookup(sid);
            if (s) username = s->username;
        }
    }

    /* Try to serve index.html */
    const char *ct;
    int file_len = http_serve_file(g_current_root, "/", body, body_size, &ct);
    if (file_len <= 0) {
        /* No index.html — show welcome page */
        if (username) {
            char initial = (username[0]>='a'?(char)(username[0]-32):username[0]);
            char page[2048];
            snprintf(page, sizeof(page),
                "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>MiniWeb</title><style>"
                "body{font-family:system-ui,sans-serif;background:#f1f5f9;margin:0;}"
                "nav{background:#fff;border-bottom:1px solid #e2e8f0;padding:0 24px;display:flex;"
                "align-items:center;justify-content:space-between;height:52px;box-shadow:0 1px 3px #0001;}"
                ".brand{font-weight:700;font-size:1.1rem;color:#6366f1;text-decoration:none;}"
                ".ui{display:flex;align-items:center;gap:10px;font-size:.85rem;}"
                ".av{width:28px;height:28px;border-radius:50%%;background:linear-gradient(135deg,#6366f1,#4f46e5);"
                "color:#fff;display:flex;align-items:center;justify-content:center;font-weight:700;}"
                ".lo{padding:5px 14px;background:#fee2e2;color:#dc2626;border:1px solid #fecaca;"
                "border-radius:7px;text-decoration:none;font-weight:500;}"
                ".hero{text-align:center;padding:80px 20px;}"
                ".hero h1{font-size:1.8rem;color:#1e293b;margin-bottom:8px;}"
                ".hero p{color:#64748b;margin-bottom:24px;}"
                ".hero a{display:inline-block;margin:4px;padding:10px 22px;background:#fff;color:#1e293b;"
                "text-decoration:none;border-radius:10px;font-weight:500;border:1px solid #e2e8f0;}"
                ".hero a:hover{background:#f1f5f9;}"
                "</style></head><body>\n"
                "<nav><a href=\"/\" class=\"brand\">⚡ MiniWeb</a>"
                "<div class=\"ui\"><div class=\"av\">%c</div><span>%s</span>"
                "<a href=\"/logout\" class=\"lo\">Logout</a></div></nav>\n"
                "<div class=\"hero\"><h1>Welcome, %s!</h1>"
                "<p><a href=\"/users\">📋 User List</a><a href=\"/search\">🔍 Search</a>"
                "<a href=\"/hello\">👋 Hello</a></p></div></body></html>\n",
                initial, username, username);
            return http_build_response(200, "OK", "text/html; charset=utf-8",
                                       page, (int)strlen(page),
                                       req->keep_alive>0?req->keep_alive:1, output, output_size);
        }
        const char *welcome =
            "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>MiniWeb</title><style>"
            "body{font-family:system-ui,sans-serif;background:linear-gradient(135deg,#0f172a,#1e293b,#334155);"
            "min-height:100vh;display:flex;align-items:center;justify-content:center;text-align:center;}"
            ".hero{color:#fff;}.hero h1{font-size:2.5rem;margin-bottom:12px;}"
            ".hero p{color:#94a3b8;margin-bottom:32px;}"
            ".hero a{padding:14px 36px;background:linear-gradient(135deg,#6366f1,#4f46e5);color:#fff;"
            "text-decoration:none;border-radius:12px;font-weight:600;}"
            "</style></head><body><div class=\"hero\"><h1>⚡ MiniWeb Server v1.7</h1>"
            "<p>Lightweight HTTP Server · Session/Cookie Auth</p>"
            "<a href=\"/login\">Sign In</a></div></body></html>\n";
        return http_build_response(200, "OK", "text/html; charset=utf-8",
                                   welcome, (int)strlen(welcome),
                                   req->keep_alive>0?req->keep_alive:1, output, output_size);
    }

    /* Resolve display name: for CSV users (mobile login), look up real name */
    const char *display_name = username;
    if (username) {
        extern const char *user_store_lookup_name(const char *mobile);
        const char *real = user_store_lookup_name(username);
        if (real) display_name = real;
    }

    /* Has index.html — inject nav bar if logged in, otherwise serve as-is */
    if (!username) {
        /* Add Cache-Control to prevent browser caching old version */
        char *nocache = output;
        int hdr_len = snprintf(nocache, output_size,
            "HTTP/1.1 200 OK\r\n"
            "Server: MiniWeb/1.7\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "\r\n",
            ct, file_len, req->keep_alive>0?"Keep-Alive":"close");
        if (hdr_len > 0 && hdr_len < output_size) {
            int remaining = output_size - hdr_len;
            if (file_len < remaining) {
                memcpy(output + hdr_len, body, file_len);
                return hdr_len + file_len;
            }
        }
        return http_build_response(200, "OK", ct, body, file_len,
                                   req->keep_alive>0?req->keep_alive:1, output, output_size);
    }

    /* Logged in: inject nav bar after <body...> tag */
    const char *body_tag = body;
    while (*body_tag && strncasecmp(body_tag, "<body", 5) != 0) body_tag++;
    if (!*body_tag) body_tag = strstr(body, "<BODY");
    if (body_tag) {
        const char *tag_end = strchr(body_tag, '>');
        if (tag_end) {
            int head_len = (int)(tag_end + 1 - body);       /* up to and including > */
            int tail_len = file_len - head_len;              /* rest of page content */
            char initial = (username[0]>='a'?(char)(username[0]-32):username[0]);
            /* Build nav HTML */
            char nav[2048]; int nav_len = 0;
            #define NADD(s) do { int l=(int)strlen(s); if(nav_len+l<2048){memcpy(nav+nav_len,s,l);nav_len+=l;} } while(0)
            /* Simple compact nav: brand | Users Search | avatar name Logout */
            NADD("<style>.mwnav{background:#1e293b;padding:0 24px;display:flex;"
                "align-items:center;justify-content:space-between;height:48px;"
                "font-family:system-ui,sans-serif;box-shadow:0 2px 6px rgba(0,0,0,0.2);}"
                ".mwnav a{text-decoration:none;transition:opacity .2s;}"
                ".mwnav .brand{font-weight:800;font-size:17px;color:#f8fafc;}"
                ".mwnav .link{padding:6px 12px;color:#94a3b8;font-size:13px;font-weight:500;border-radius:6px;}"
                ".mwnav .link:hover{color:#f8fafc;background:rgba(255,255,255,0.08);}"
                ".mwnav .av{width:30px;height:30px;border-radius:50%;"
                "background:linear-gradient(135deg,#6366f1,#818cf8);color:#fff;"
                "display:inline-flex;align-items:center;justify-content:center;"
                "font-weight:700;font-size:13px;box-shadow:0 2px 6px rgba(99,102,241,0.4);}"
                ".mwnav .uname{color:#e2e8f0;font-size:13px;font-weight:500;}"
                ".mwnav .lo{padding:6px 16px;background:rgba(255,255,255,0.1);color:#f1f5f9;"
                "border:1px solid rgba(255,255,255,0.2);border-radius:8px;font-size:12px;}"
                ".mwnav .lo:hover{background:#dc2626;border-color:#dc2626;}"
                "</style>\n<div class=\"mwnav\">\n"
                "<div style=\"display:flex;align-items:center;gap:18px;\">\n"
                "<a href=\"/\" class=\"brand\">⚡ MiniWeb</a>\n"
                "<a href=\"/users\" class=\"link\">Users</a>\n"
                "<a href=\"/search\" class=\"link\">Search</a>\n"
                "</div>\n"
                "<div style=\"display:flex;align-items:center;gap:10px;\">\n"
                "<span class=\"av\">");
            nav[nav_len++] = initial;
            NADD("</span>\n<span class=\"uname\">");
            { const char *dn = display_name ? display_name : username;
              int ul=(int)strlen(dn); if(nav_len+ul<2048){memcpy(nav+nav_len,dn,ul);nav_len+=ul;} }
            NADD("</span>\n<a href=\"/logout\" class=\"lo\">Logout</a>\n"
                "</div>\n</div>\n");
            #undef NADD
            /* Make room in body buffer: shift tail content to make space for nav */
            if (head_len + nav_len + tail_len < body_size) {
                memmove(body + head_len + nav_len, body + head_len, tail_len);
                memcpy(body + head_len, nav, nav_len);
                file_len = head_len + nav_len + tail_len;
                body[file_len] = '\0';
            }
        }
    }
    return http_build_response(200, "OK", ct, body, file_len,
                               req->keep_alive>0?req->keep_alive:1, output, output_size);
}

/* ---- User CRUD handlers ---- */

static int handle_user_list(const request_t *req, char *body, int body_size,
                             const char *captured, char *output, int output_size) {
    (void)captured;
    int total = 0;
    int offset = 0;
    (void)req;
    user_store_format_users(body, body_size - 1, &total, &offset);
    if (offset >= body_size - 200) {
        offset += snprintf(body + offset, body_size - offset,
                 "\n... (showing first entries out of %d total users)\n", total);
    }
    return build_http_response(output, output_size, 200, "OK", body);
}

static int handle_user_find_index(const request_t *req, char *body, int body_size,
                                   const char *captured, char *output,
                                   int output_size) {
    (void)req;
    const char *name = (captured && captured[0]) ? captured : "";
    url_decode_path((char *)name);
    ListNode *user = user_store_find_index(name);
    if (user != NULL) {
        if (format_user_info(user, body, body_size) < 0) return -1;
        return build_http_response(output, output_size, 200, "OK", body);
    }
    snprintf(body, body_size, "NOT_FOUND %s\n", name);
    return build_http_response(output, output_size, 404, "NOT FOUND", body);
}

static int handle_user_compare_verbose(const request_t *req, char *body,
                                        int body_size, const char *captured,
                                        char *output, int output_size) {
    (void)req;
    const char *name = (captured && captured[0]) ? captured : "";
    url_decode_path((char *)name);
    g_compare_ctx.name = name;
    g_compare_ctx.verbose = 1;
    if (capture_stdout(do_compare_search_wrapper, body, body_size) < 0) {
        return build_http_response(output, output_size, 500,
                                   "INTERNAL SERVER ERROR",
                                   "500 Internal Server Error\n");
    }
    return build_http_response(output, output_size, 200, "OK", body);
}

static int handle_user_compare(const request_t *req, char *body, int body_size,
                                const char *captured, char *output,
                                int output_size) {
    (void)req;
    const char *name = (captured && captured[0]) ? captured : "";
    url_decode_path((char *)name);
    g_compare_ctx.name = name;
    g_compare_ctx.verbose = 0;
    if (capture_stdout(do_compare_search_wrapper, body, body_size) < 0) {
        return build_http_response(output, output_size, 500,
                                   "INTERNAL SERVER ERROR",
                                   "500 Internal Server Error\n");
    }
    return build_http_response(output, output_size, 200, "OK", body);
}

static int handle_user_by_name(const request_t *req, char *body, int body_size,
                                const char *captured, char *output,
                                int output_size) {
    (void)req;
    const char *name = (captured && captured[0]) ? captured : "";
    url_decode_path((char *)name);
    if (name[0] == '\0') {
        return build_http_response(output, output_size, 400, "BAD REQUEST",
                                   "ERROR: empty user name\n");
    }
    {
        ListNode *user = user_store_find(name);
        if (user != NULL) {
            if (format_user_info(user, body, body_size) < 0) return -1;
            return build_http_response(output, output_size, 200, "OK", body);
        }
        snprintf(body, body_size, "NOT_FOUND %s\n", name);
        return build_http_response(output, output_size, 404, "NOT FOUND", body);
    }
}

static int handle_user_simple_find(const request_t *req, char *body,
                                    int body_size, const char *captured,
                                    char *output, int output_size) {
    (void)req;
    const char *name = (captured && captured[0]) ? captured : "";
    url_decode_path((char *)name);
    if (name[0] == '\0') {
        return build_http_response(output, output_size, 400, "BAD REQUEST",
                                   "ERROR: empty user name\n");
    }
    {
        ListNode *user = user_store_find(name);
        if (user != NULL) {
            if (format_user_info(user, body, body_size) < 0) return -1;
            return build_http_response(output, output_size, 200, "OK", body);
        }
        snprintf(body, body_size, "NOT_FOUND %s\n", name);
        return build_http_response(output, output_size, 404, "NOT FOUND", body);
    }
}

static int handle_user_add(const request_t *req, char *body, int body_size,
                            const char *captured, char *output, int output_size) {
    (void)captured;
    int ret;
    if (req->body[0] == '\0') {
        return build_http_response(output, output_size, 400, "BAD REQUEST",
            "ERROR: POST /users requires a body with CSV data\n");
    }
    ret = user_store_add(req->body);
    if (ret == 0) {
        return build_http_response(output, output_size, 200, "OK", "ADDED\n");
    }
    return build_http_response(output, output_size, 200, "OK", "EXISTS\n");
}

static int handle_user_delete(const request_t *req, char *body, int body_size,
                               const char *captured, char *output,
                               int output_size) {
    (void)req;
    const char *name = (captured && captured[0]) ? captured : "";
    url_decode_path((char *)name);
    int ret;
    if (name[0] == '\0') {
        return build_http_response(output, output_size, 400, "BAD REQUEST",
                                   "ERROR: DELETE requires a user name\n");
    }
    ret = user_store_delete(name);
    if (ret == 0) {
        return build_http_response(output, output_size, 200, "OK", "DELETED\n");
    }
    return build_http_response(output, output_size, 404, "NOT FOUND",
                               "NO_SUCH_USER\n");
}

static int handle_delete_form(const request_t *req, char *body, int body_size,
                               const char *captured, char *output,
                               int output_size) {
    (void)captured;
    char name_buf[64];
    const char *n_ptr;
    int ret;

    memset(name_buf, 0, sizeof(name_buf));
    n_ptr = strstr(req->body, "name=");
    if (n_ptr != NULL) {
        n_ptr += 5;
        url_decode((char *)n_ptr, name_buf, sizeof(name_buf));
    }

    if (name_buf[0] == '\0') {
        return build_http_response(output, output_size, 400, "BAD REQUEST",
                                   "ERROR: DELETE requires a user name\n");
    }
    ret = user_store_delete(name_buf);
    if (ret == 0) {
        return build_http_response(output, output_size, 200, "OK", "DELETED\n");
    }
    return build_http_response(output, output_size, 404, "NOT FOUND",
                               "NO_SUCH_USER\n");
}

static int handle_search(const request_t *req, char *body, int body_size,
                          const char *captured, char *output, int output_size) {
    (void)captured;

    /* GET /search → return search form page */
    if (strcmp(req->method, "GET") == 0) {
        const char *form =
            "<!DOCTYPE html>\n<html lang=\"en\"><head>\n<meta charset=\"utf-8\">\n"
            "<title>Search — MiniWeb</title>\n<style>\n"
            "*{margin:0;padding:0;box-sizing:border-box;}\n"
            "body{font-family:system-ui,sans-serif;background:#f1f5f9;min-height:100vh;}\n"
            "nav{background:#fff;border-bottom:1px solid #e2e8f0;padding:0 24px;display:flex;"
            "align-items:center;justify-content:space-between;height:56px;box-shadow:0 1px 3px #0001;}\n"
            ".brand{font-weight:700;font-size:1.1rem;color:#6366f1;text-decoration:none;}\n"
            ".nav-right a{padding:6px 16px;background:#fee2e2;color:#dc2626;border:1px solid #fecaca;"
            "border-radius:8px;font-size:.82rem;text-decoration:none;font-weight:500;}\n"
            ".nav-right a:hover{background:#fecaca;}\n"
            "main{max-width:640px;margin:40px auto;padding:0 20px;}\n"
            ".card{background:#fff;border-radius:16px;padding:32px;box-shadow:0 1px 3px #0001;}\n"
            "h1{font-size:1.3rem;margin-bottom:20px;color:#1e293b;}\n"
            ".field{margin-bottom:14px;}\n"
            ".field label{display:block;font-size:.85rem;color:#64748b;margin-bottom:4px;}\n"
            ".field input{width:100%;padding:10px 14px;border:2px solid #e2e8f0;border-radius:10px;font-size:.95rem;"
            "outline:none;transition:border-color .2s;}\n"
            ".field input:focus{border-color:#6366f1;}\n"
            "button{width:100%;padding:12px;background:linear-gradient(135deg,#6366f1,#4f46e5);color:#fff;"
            "border:none;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;}\n"
            "button:hover{opacity:.92;}\n"
            ".back{margin-top:16px;text-align:center;}\n"
            ".back a{color:#6366f1;text-decoration:none;font-size:.85rem;}\n"
            "</style></head><body>\n<nav>\n<a href=\"/\" class=\"brand\">⚡ MiniWeb</a>\n"
            "<div class=\"nav-right\"><a href=\"/logout\">Logout</a></div>\n</nav>\n"
            "<main>\n<div class=\"card\">\n<h1>🔍 User Search</h1>\n"
            "<form method=\"post\" action=\"/search\" enctype=\"application/x-www-form-urlencoded\">\n"
            "<div class=\"field\"><label>Name</label><input type=\"text\" name=\"name\" placeholder=\"e.g. 赵安\" autofocus></div>\n"
            "<div class=\"field\"><label>Phone</label><input type=\"text\" name=\"phone\" placeholder=\"e.g. 138\"></div>\n"
            "<div class=\"field\"><label>Email</label><input type=\"text\" name=\"email\" placeholder=\"e.g. @example.com\"></div>\n"
            "<button type=\"submit\">Search</button>\n</form>\n"
            "<div class=\"back\"><a href=\"/\">← Back to Dashboard</a></div>\n"
            "</div>\n</main>\n</body></html>\n";
        return http_build_response(200, "OK", "text/html; charset=utf-8",
                                   form, (int)strlen(form),
                                   req->keep_alive > 0 ? req->keep_alive : 1,
                                   output, output_size);
    }

    /* POST /search → process search (existing logic) */
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
                                   body, (int)strlen(body), 0, output, output_size);
    }

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
                                   body, (int)strlen(body), 0, output, output_size);
    }

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
                                   body, (int)strlen(body), 0, output, output_size);
    }

    {
        search_criteria_t criteria;
        char name_buf[64], phone_buf[32], email_buf[64], mobile_buf[32];
        char *p_ptr;
        int match_count;
        int has_criteria = 0;

        memset(&criteria, 0, sizeof(criteria));
        memset(name_buf, 0, sizeof(name_buf));
        memset(phone_buf, 0, sizeof(phone_buf));
        memset(email_buf, 0, sizeof(email_buf));
        memset(mobile_buf, 0, sizeof(mobile_buf));

        p_ptr = strstr(req->body, "name=");
        if (p_ptr != NULL) {
            p_ptr += 5;
            if (url_decode(p_ptr, name_buf, sizeof(name_buf)) != 0) {
                return build_http_response(output, output_size, 400,
                    "BAD REQUEST", "URL encoding error in name parameter.\n");
            }
            if (name_buf[0] != '\0') {
                strncpy(criteria.name, name_buf, sizeof(criteria.name) - 1);
                has_criteria = 1;
            }
        }

        p_ptr = strstr(req->body, "phone=");
        if (p_ptr != NULL) {
            p_ptr += 6;
            if (url_decode(p_ptr, phone_buf, sizeof(phone_buf)) != 0) {
                return build_http_response(output, output_size, 400,
                    "BAD REQUEST", "URL encoding error in phone parameter.\n");
            }
            if (phone_buf[0] != '\0') {
                if (!validate_phone(phone_buf)) {
                    return build_http_response(output, output_size, 400,
                        "BAD REQUEST", "Invalid phone format.\n");
                }
                strncpy(criteria.phone, phone_buf, sizeof(criteria.phone) - 1);
                has_criteria = 1;
            }
        }

        p_ptr = strstr(req->body, "mobile=");
        if (p_ptr != NULL) {
            p_ptr += 7;
            if (url_decode(p_ptr, mobile_buf, sizeof(mobile_buf)) != 0) {
                return build_http_response(output, output_size, 400,
                    "BAD REQUEST", "URL encoding error in mobile parameter.\n");
            }
            if (mobile_buf[0] != '\0') {
                if (!validate_phone(mobile_buf)) {
                    return build_http_response(output, output_size, 400,
                        "BAD REQUEST", "Invalid mobile format.\n");
                }
                strncpy(criteria.mobile, mobile_buf, sizeof(criteria.mobile) - 1);
                has_criteria = 1;
            }
        }

        p_ptr = strstr(req->body, "email=");
        if (p_ptr != NULL) {
            p_ptr += 6;
            if (url_decode(p_ptr, email_buf, sizeof(email_buf)) != 0) {
                return build_http_response(output, output_size, 400,
                    "BAD REQUEST", "URL encoding error in email parameter.\n");
            }
            if (email_buf[0] != '\0') {
                if (!validate_email(email_buf)) {
                    return build_http_response(output, output_size, 400,
                        "BAD REQUEST", "Invalid email format.\n");
                }
                strncpy(criteria.email, email_buf, sizeof(criteria.email) - 1);
                has_criteria = 1;
            }
        }

        if (!has_criteria) {
            /* Return search form HTML (simplified) */
            snprintf(body, body_size,
                "<!DOCTYPE html>\n"
                "<html lang=\"en\"><head>\n"
                "<meta charset=\"utf-8\">\n"
                "<title>User Search</title>\n"
                "<style>"
                "body{font-family:sans-serif;max-width:640px;margin:48px auto;"
                "padding:0 16px;}"
                "</style></head><body>\n"
                "<h1>User Search</h1>\n"
                "<form method=\"post\" action=\"/search\""
                " enctype=\"application/x-www-form-urlencoded\">\n"
                "<p><label>Name: <input type=\"text\" name=\"name\"></label></p>\n"
                "<p><label>Phone: <input type=\"text\" name=\"phone\"></label></p>\n"
                "<p><label>Email: <input type=\"text\" name=\"email\"></label></p>\n"
                "<p><button type=\"submit\">Search</button></p>\n"
                "</form>\n"
                "<p>Please enter at least one search criterion.</p>\n"
                "</body></html>\n");
            return http_build_response(200, "OK", "text/html; charset=utf-8",
                                       body, (int)strlen(body),
                                       req->keep_alive > 0 ? req->keep_alive : 1,
                                       output, output_size);
        }

        { int capped = body_size; if (capped > 2*1024*1024) capped = 2*1024*1024;
          match_count = user_store_search(&criteria, body, capped); }
        if (match_count < 0) {
            return http_build_response(500, "INTERNAL SERVER ERROR",
                                       "text/plain; charset=utf-8",
                                       "500 Internal Server Error\n", 25,
                                       0, output, output_size);
        }
	if (match_count >= 500) {
	    int blen = (int)strlen(body);
	    snprintf(body + blen, body_size - blen,
	             "\n<p style=\"color:#dc2626;text-align:center;\">"
	             "⚠ Showing first 500 of %d+ matches. Narrow your search.</p>\n",
	             match_count);
	}

        return http_build_response(200, "OK", "text/html; charset=utf-8",
                                   body, (int)strlen(body),
                                   req->keep_alive > 0 ? req->keep_alive : 1,
                                   output, output_size);
    }
}

/* ---- Blog / static file handlers ---- */

static int handle_blog(const request_t *req, char *body, int body_size,
                        const char *captured, char *output, int output_size) {
    (void)captured;

    if (!http_is_safe_path(req->path)) {
        snprintf(body, body_size,
            "<!DOCTYPE html>\n"
            "<html><head><title>403 Forbidden</title></head>\n"
            "<body><h1>403 Forbidden</h1></body></html>\n");
        return http_build_response(403, "FORBIDDEN",
                                   "text/html; charset=utf-8",
                                   body, (int)strlen(body), 0, output, output_size);
    }

    /* 301: /blog → /blog/ */
    if (strcmp(req->path, "/blog") == 0) {
        return http_build_redirect(301, "/blog/", output, output_size);
    }

    {
        const char *ct;
        const char *blog_path = req->path + 5;  /* skip "/blog" */
        int file_len = http_serve_file("blog", blog_path, body, body_size, &ct);
        if (file_len > 0) {
            return http_build_response(200, "OK", ct, body, file_len,
                                       req->keep_alive > 0 ? req->keep_alive : 1,
                                       output, output_size);
        }

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
                                   output, output_size);
    }
}

static int handle_static(const request_t *req, char *body, int body_size,
                          const char *captured, char *output, int output_size) {
    (void)captured;

    if (!http_is_safe_path(req->path)) {
        snprintf(body, body_size,
            "<!DOCTYPE html>\n"
            "<html><head><title>403 Forbidden</title></head>\n"
            "<body><h1>403 Forbidden</h1></body></html>\n");
        return http_build_response(403, "FORBIDDEN",
                                   "text/html; charset=utf-8",
                                   body, (int)strlen(body), 0, output, output_size);
    }

    {
        const char *ct;
        int file_len = http_serve_file(g_current_root, req->path,
                                       body, body_size, &ct);
        if (file_len > 0) {
            return http_build_response(200, "OK", ct, body, file_len,
                                       req->keep_alive > 0 ? req->keep_alive : 1,
                                       output, output_size);
        }
    }

    return -1;  /* signal: file not found, caller handles 404 */
}

/* ---- v1.7: Login / Logout handlers ---- */

static int handle_login(const request_t *req, char *body, int body_size,
                         const char *captured, char *output, int output_size) {
    (void)captured;

    /* GET /login → return styled HTML login form */
    if (strcmp(req->method, "GET") == 0) {
        const char *form =
            "<!DOCTYPE html>\n"
            "<html lang=\"en\"><head>\n"
            "<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
            "<title>Login — MiniWeb Server v1.7</title>\n"
            "<style>\n"
            "*{margin:0;padding:0;box-sizing:border-box;}\n"
            "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
            "background:linear-gradient(135deg,#0f172a 0%,#1e293b 50%,#334155 100%);"
            "min-height:100vh;display:flex;align-items:center;justify-content:center;}\n"
            ".container{width:100%;max-width:400px;padding:20px;}\n"
            ".logo{text-align:center;margin-bottom:32px;}\n"
            ".logo h1{font-size:1.8rem;color:#f8fafc;font-weight:700;letter-spacing:-0.5px;}\n"
            ".logo p{color:#94a3b8;font-size:0.9rem;margin-top:6px;}\n"
            ".card{background:#fff;border-radius:16px;padding:40px;box-shadow:0 25px 50px rgba(0,0,0,0.3);}\n"
            ".card h2{font-size:1.25rem;color:#0f172a;margin-bottom:24px;font-weight:600;}\n"
            ".field{margin-bottom:16px;}\n"
            ".field label{display:block;font-size:0.82rem;color:#64748b;margin-bottom:6px;font-weight:500;}\n"
            ".field input{width:100%;padding:12px 16px;border:2px solid #e2e8f0;border-radius:10px;"
            "font-size:0.95rem;color:#0f172a;transition:border-color .2s;outline:none;}\n"
            ".field input:focus{border-color:#6366f1;}\n"
            ".field input::placeholder{color:#cbd5e1;}\n"
            "button{width:100%;padding:14px;background:linear-gradient(135deg,#6366f1,#4f46e5);"
            "color:#fff;border:none;border-radius:10px;font-size:1rem;font-weight:600;"
            "cursor:pointer;transition:opacity .2s,transform .1s;margin-top:8px;}\n"
            "button:hover{opacity:0.92;}\n"
            "button:active{transform:scale(0.98);}\n"
            ".error{background:#fef2f2;color:#dc2626;padding:12px 16px;border-radius:10px;"
            "font-size:0.85rem;margin-bottom:16px;border:1px solid #fecaca;display:none;}\n"
            ".footer{text-align:center;margin-top:24px;color:#94a3b8;font-size:0.78rem;}\n"
            "</style></head><body>\n"
            "<div class=\"container\">\n"
            "<div class=\"logo\">\n"
            "<h1>⚡ MiniWeb</h1>\n"
            "<p>Lightweight HTTP Server v1.7</p>\n"
            "</div>\n"
            "<div class=\"card\">\n"
            "<h2>Sign in to continue</h2>\n"
            "<div class=\"error\" id=\"error\">Invalid username or password</div>\n"
            "<form method=\"post\" action=\"/login\">\n"
            "<div class=\"field\">"
            "<label>Username</label>"
            "<input type=\"text\" name=\"username\" placeholder=\"Enter your username\" required autofocus>"
            "</div>\n"
            "<div class=\"field\">"
            "<label>Password</label>"
            "<input type=\"password\" name=\"password\" placeholder=\"Enter your password\" required>"
            "</div>\n"
            "<button type=\"submit\">Sign In</button>\n"
            "</form>\n</div>\n"
            "<div class=\"footer\">Session Cookie Authentication · HttpOnly</div>\n"
            "</div>\n</body></html>\n";
        return http_build_response(200, "OK", "text/html; charset=utf-8",
                                   form, (int)strlen(form), 0, output, output_size);
    }

    /* POST /login → authenticate and set session cookie */
    if (strcmp(req->method, "POST") == 0) {
        char username[64] = "", password[64] = "";
        const char *u = strstr(req->body, "username=");
        const char *p = strstr(req->body, "password=");
        if (u) { u += 9; int i = 0; while (*u && *u != '&' && i < 63) username[i++] = *u++; username[i] = '\0'; }
        if (p) { p += 9; int i = 0; while (*p && *p != '&' && i < 63) password[i++] = *p++; password[i] = '\0'; }

        const char *user_role = auth_lookup(username, password);
        if (user_role) {
            const char *sid = session_create(username, user_role);
            if (sid) {
                return snprintf(output, output_size,
                    "HTTP/1.1 302 Found\r\n"
                    "Server: MiniWeb/1.7\r\n"
                    "Set-Cookie: session_id=%s; Path=/; HttpOnly; Max-Age=%d; SameSite=Lax\r\n"
                    "Location: /\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: Keep-Alive\r\n"
                    "\r\n", sid, SESSION_TTL);
            }
        }
        /* Auth failed → show login page with error */
        {
            const char *fail =
                "<!DOCTYPE html>\n"
                "<html lang=\"en\"><head>\n"
                "<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                "<title>Login Failed — MiniWeb</title>\n"
                "<style>\n"
                "*{margin:0;padding:0;box-sizing:border-box;}\n"
                "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
                "background:linear-gradient(135deg,#0f172a 0 P0,#1e293b 50 P0,#334155 100 P0);"
                "min-height:100vh;display:flex;align-items:center;justify-content:center;}\n"
                ".container{width:100 P0;max-width:420px;padding:20px;text-align:center;}\n"
                ".card{background:#fff;border-radius:16px;padding:48px 40px;box-shadow:0 25px 50px rgba(0,0,0,0.3);}\n"
                ".icon{font-size:3rem;margin-bottom:16px;}\n"
                "h1{color:#dc2626;font-size:1.3rem;margin-bottom:8px;}\n"
                "p{color:#64748b;font-size:0.9rem;margin-bottom:24px;}\n"
                "a{display:inline-block;padding:12px 32px;background:linear-gradient(135deg,#6366f1,#4f46e5);"
                "color:#fff;text-decoration:none;border-radius:10px;font-weight:600;font-size:0.9rem;}\n"
                "a:hover{opacity:0.9;}\n"
                "</style></head><body>\n"
                "<div class=\"container\"><div class=\"card\">\n"
                "<div class=\"icon\">&#x1f512;</div>\n"
                "<h1>Login Failed</h1>\n"
                "<p>Invalid username or password. Please try again.</p>\n"
                "<a href=\"/login\">Try Again</a>\n"
                "</div></div></body></html>\n";
            return http_build_response(401, "UNAUTHORIZED", "text/html; charset=utf-8",
                                       fail, (int)strlen(fail), 0, output, output_size);
        }
    }

    return build_http_response(output, output_size, 405, "METHOD NOT ALLOWED",
                               "405 Method Not Allowed\n");
}

static int handle_logout(const request_t *req, char *body, int body_size,
                          const char *captured, char *output, int output_size) {
    (void)body; (void)body_size; (void)captured;

    /* GET or POST — both log out */

    /* Extract session_id from cookie */
    const char *cookie = req->cookie;
    const char *sid_start = strstr(cookie, "session_id=");
    if (sid_start) {
        sid_start += 11;
        char sid[SESSION_ID_LEN];
        int i = 0;
        while (sid_start[i] && sid_start[i] != ';' && sid_start[i] != ' '
               && i < (int)sizeof(sid) - 1) {
            sid[i] = sid_start[i]; i++;
        }
        sid[i] = '\0';
        session_destroy(sid);
    }

    /* Clear cookie + redirect to login */
    return snprintf(output, output_size,
        "HTTP/1.1 302 Found\r\n"
        "Server: MiniWeb/1.7\r\n"
        "Set-Cookie: session_id=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax\r\n"
        "Location: /login\r\n"
        "Content-Length: 0\r\n"
        "Connection: Keep-Alive\r\n"
        "\r\n");
}

/* ---- Handler registry (maps handler_type_t → handler_fn) ---- */

typedef struct {
    handler_type_t type;
    handler_fn     fn;
} handler_reg_t;

static const handler_reg_t g_handler_registry[] = {
    { HANDLER_HELLO,                handle_hello                },
    { HANDLER_HELP,                 handle_help                 },
    { HANDLER_SLEEP,                handle_sleep                },
    { HANDLER_INDEX,                handle_index                },
    { HANDLER_USER_LIST,            handle_user_list            },
    { HANDLER_USER_FIND_INDEX,      handle_user_find_index      },
    { HANDLER_USER_COMPARE_VERBOSE, handle_user_compare_verbose },
    { HANDLER_USER_COMPARE,         handle_user_compare         },
    { HANDLER_USER_BY_NAME,         handle_user_by_name         },
    { HANDLER_USER_SIMPLE_FIND,     handle_user_simple_find     },
    { HANDLER_USER_ADD,             handle_user_add             },
    { HANDLER_USER_DELETE,          handle_user_delete          },
    { HANDLER_DELETE_FORM,          handle_delete_form          },
    { HANDLER_SEARCH,               handle_search               },
    { HANDLER_BLOG,                 handle_blog                 },
    { HANDLER_STATIC,               handle_static               },
    { HANDLER_LOGIN,                handle_login                },
    { HANDLER_LOGOUT,               handle_logout               },
    { HANDLER_NONE,                 NULL                        }  /* sentinel */
};

handler_fn route_table_get_handler_fn(handler_type_t type) {
    int i;
    for (i = 0; g_handler_registry[i].type != HANDLER_NONE; i++) {
        if (g_handler_registry[i].type == type) {
            return g_handler_registry[i].fn;
        }
    }
    return NULL;
}

/*
 * Populate the route table with default routes matching the existing
 * hardcoded behavior.  Called when no routes were configured.
 */
static void route_table_populate_defaults(route_table_t *rt) {
    /* v1.5: one entry per (method, path).  Exact routes first, then prefix. */

    /* ---- Exact routes ---- */
    route_table_add(rt, "GET",    "/hello",   MATCH_EXACT, HANDLER_HELLO, "", "");
    route_table_add(rt, "GET",    "/help",    MATCH_EXACT, HANDLER_HELP, "", "");
    route_table_add(rt, "GET",    "/users",   MATCH_EXACT, HANDLER_USER_LIST, "", "");
    route_table_add(rt, "POST",   "/users",   MATCH_EXACT, HANDLER_USER_ADD, "", "");
    route_table_add(rt, "GET",    "/search",  MATCH_EXACT, HANDLER_SEARCH, "", "");
    route_table_add(rt, "POST",   "/search",  MATCH_EXACT, HANDLER_SEARCH, "", "");
    route_table_add(rt, "GET",    "/logout",  MATCH_EXACT, HANDLER_LOGOUT, "", "");
    route_table_add(rt, "POST",   "/logout",  MATCH_EXACT, HANDLER_LOGOUT, "", "");
    route_table_add(rt, "POST",   "/delete",  MATCH_EXACT, HANDLER_DELETE_FORM, "", "");
    route_table_add(rt, "GET",    "/blog",    MATCH_EXACT, HANDLER_BLOG, "", "");
    route_table_add(rt, "HEAD",   "/blog",    MATCH_EXACT, HANDLER_BLOG, "", "");
    route_table_add(rt, "GET",    "/",        MATCH_EXACT, HANDLER_INDEX, "", "");
    route_table_add(rt, "HEAD",   "/",        MATCH_EXACT, HANDLER_INDEX, "", "");

    /* ---- Prefix routes (longer prefixes first) ---- */
    route_table_add(rt, "GET",    "/users/find-index/",      MATCH_PREFIX, HANDLER_USER_FIND_INDEX, "", "");
    route_table_add(rt, "GET",    "/users/compare-verbose/", MATCH_PREFIX, HANDLER_USER_COMPARE_VERBOSE, "", "");
    route_table_add(rt, "GET",    "/users/compare/",         MATCH_PREFIX, HANDLER_USER_COMPARE, "", "");
    route_table_add(rt, "GET",    "/users/",                 MATCH_PREFIX, HANDLER_USER_BY_NAME, "", "");
    route_table_add(rt, "DELETE", "/users/",                 MATCH_PREFIX, HANDLER_USER_DELETE, "", "");
    route_table_add(rt, "GET",    "/user/",                  MATCH_PREFIX, HANDLER_USER_SIMPLE_FIND, "", "");
    route_table_add(rt, "GET",    "/sleep/",                 MATCH_PREFIX, HANDLER_SLEEP, "", "");
    route_table_add(rt, "GET",    "/blog/",                  MATCH_PREFIX, HANDLER_BLOG, "", "");
    route_table_add(rt, "HEAD",   "/blog/",                  MATCH_PREFIX, HANDLER_BLOG, "", "");

    /* ---- Static file fallback (lowest priority) ---- */
    route_table_add(rt, "GET",    "/", MATCH_PREFIX, HANDLER_STATIC, "", "");
    route_table_add(rt, "HEAD",   "/", MATCH_PREFIX, HANDLER_STATIC, "", "");
}

/* ================================================================
 *  v1.6: HTTP Basic Auth credential store
 * ================================================================ */

/* External: base64 decoder */
extern int base64_decode(const char *src, char *dst, int dst_size);

#define MAX_AUTH_USERS 64

typedef struct {
    char username[64];
    char password[64];
    char role[32];
} auth_user_t;

static auth_user_t g_auth_users[MAX_AUTH_USERS];
static int g_auth_user_count = 0;

int auth_load_file(const char *path) {
    FILE *fp = fopen(path, "r");
    char line[256];
    if (!fp) return -1;

    g_auth_user_count = 0;
    while (fgets(line, sizeof(line), fp) && g_auth_user_count < MAX_AUTH_USERS) {
        char *nl = strchr(line, '\r'); if (nl) *nl = '\0';
        nl = strchr(line, '\n'); if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        /* Format: username:password:role */
        char *first_colon  = strchr(line, ':');
        char *second_colon = first_colon ? strchr(first_colon + 1, ':') : NULL;
        if (!first_colon || !second_colon) continue;

        *first_colon  = '\0';
        *second_colon = '\0';

        auth_user_t *u = &g_auth_users[g_auth_user_count];
        strncpy(u->username, line, sizeof(u->username) - 1);
        u->username[sizeof(u->username) - 1] = '\0';
        strncpy(u->password, first_colon + 1, sizeof(u->password) - 1);
        u->password[sizeof(u->password) - 1] = '\0';
        strncpy(u->role, second_colon + 1, sizeof(u->role) - 1);
        u->role[sizeof(u->role) - 1] = '\0';

        g_auth_user_count++;
    }
    fclose(fp);
    log_infof("Auth: loaded %d user(s) from %s", g_auth_user_count, path);
    return 0;
}

/*
 * Look up a user, return their role string (or NULL if not found).
 * First checks .htpasswd, then falls back to CSV user store.
 * CSV users use mobile as username and have role "user".
 */
static const char *auth_lookup(const char *username, const char *password) {
    int i;
    /* 1. Check .htpasswd file */
    for (i = 0; i < g_auth_user_count; i++) {
        if (strcmp(g_auth_users[i].username, username) == 0 &&
            strcmp(g_auth_users[i].password, password) == 0) {
            return g_auth_users[i].role;
        }
    }
    /* 2. Fallback: check CSV user store (mobile + password → role "user") */
    {
        extern int user_store_auth(const char *mobile, const char *pwd);
        if (user_store_auth(username, password)) {
            return "user";
        }
    }
    return NULL;
}

/*
 * Check if a user's role satisfies the required role.
 * Admin role ("admin") is a super-role that passes any check.
 */
static int auth_role_matches(const char *user_role, const char *required_role) {
    if (!user_role || !required_role) return 0;
    if (required_role[0] == '\0') return 1;  /* no role required */
    if (strcmp(user_role, "admin") == 0) return 1;  /* admin can access anything */
    return (strcmp(user_role, required_role) == 0);
}

/* ================================================================
 *  request_handler_process_http (v1.5: route-table-based dispatch)
 * ================================================================ */
int request_handler_process_http(const request_t *req, char *output, int size) {
    char *body = g_body_buf;
    int body_size = BLOG_BODY_MAX;
    route_find_result_t result;
    route_table_t *rt;

    if (req == NULL || output == NULL || size <= 0) {
        return -1;
    }

    memset(body, 0, body_size);

    /* v1.5: get route table (from config, or fall back to defaults) */
    rt = config_get_route_table();
    if (rt == NULL || (rt->exact_count + rt->prefix_count) == 0) {
        /* No routes configured — use defaults (backward compatible) */
        static route_table_t default_rt;
        static int default_rt_initialized = 0;
        if (!default_rt_initialized) {
            route_table_init(&default_rt);
            route_table_populate_defaults(&default_rt);
            default_rt_initialized = 1;
        }
        rt = &default_rt;
    }

    /* v1.5: dispatch via route table */
    route_table_find(rt, req->method, req->path, &result);

    if (result.is_405) {
        /* Path matched but method not allowed → 405 Method Not Allowed */
        char allow_header[512];
        snprintf(allow_header, sizeof(allow_header),
                 "HTTP/1.1 405 METHOD NOT ALLOWED\r\n"
                 "Server: MiniWeb/1.5\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: 0\r\n"
                 "Allow: %s\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 result.allow[0] ? result.allow : "GET");
        {
            int hdr_len = (int)strlen(allow_header);
            if (hdr_len < size) {
                memcpy(output, allow_header, hdr_len);
                output[hdr_len] = '\0';
                return hdr_len;
            }
        }
        return -1;
    }

    if (result.entry != NULL) {
        /* v1.6/v1.7: check authentication if this route requires a role */
        if (result.entry->required_role[0] != '\0') {
            const char *user_role = NULL;
            const char *realm = result.entry->auth_realm;
            if (realm[0] == '\0') realm = "Restricted";

            /* ---- v1.7: try Cookie session first ---- */
            if (req->cookie[0] != '\0') {
                const char *sid_start = strstr(req->cookie, "session_id=");
                if (sid_start) {
                    sid_start += 11;
                    char sid[SESSION_ID_LEN];
                    int si = 0;
                    while (sid_start[si] && sid_start[si] != ';'
                           && sid_start[si] != ' ' && si < (int)sizeof(sid) - 1)
                        sid[si++] = sid_start[si];
                    sid[si] = '\0';
                    session_t *s = session_lookup(sid);
                    if (s) user_role = s->role;
                }
            }

            /* ---- v1.6: fallback to Basic Auth ---- */
            if (!user_role) {
                const char *auth_header = req->authorization;
                if (auth_header[0] != '\0' &&
                    strncasecmp(auth_header, "Basic ", 6) == 0) {
                    char decoded[256];
                    int dlen = base64_decode(auth_header + 6, decoded, sizeof(decoded));
                    if (dlen < 0) {
                        return build_http_response(output, size, 400, "BAD REQUEST",
                                                   "400 Bad Request: invalid Base64\n");
                    }
                    char *colon = strchr(decoded, ':');
                    if (colon) {
                        *colon = '\0';
                        user_role = auth_lookup(decoded, colon + 1);
                    }
                }
            }

            /* No valid credentials */
            if (!user_role) {
                /* Only show Basic Auth popup if client explicitly sent
                 * an Authorization header (API client trying Basic Auth). */
                if (req->authorization[0] != '\0') {
                    return snprintf(output, size,
                        "HTTP/1.1 401 Unauthorized\r\n"
                        "Server: MiniWeb/1.7\r\n"
                        "WWW-Authenticate: Basic realm=\"%s\"\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n", realm);
                }
                /* Browser (no Authorization header) — redirect to login page.
                 * This avoids triggering the native Basic Auth popup. */
                return snprintf(output, size,
                    "HTTP/1.1 302 Found\r\n"
                    "Server: MiniWeb/1.7\r\n"
                    "Location: /login\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n");
            }

            /* Check role */
            if (!auth_role_matches(user_role, result.entry->required_role)) {
                const char *page =
                    "<!DOCTYPE html>\n<html lang=\"en\"><head>\n"
                    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
                    "<title>403 Forbidden — MiniWeb</title>\n<style>\n"
                    "*{margin:0;padding:0;box-sizing:border-box;}\n"
                    "body{font-family:system-ui,sans-serif;background:linear-gradient(135deg,#1e293b,#0f172a);"
                    "min-height:100vh;display:flex;align-items:center;justify-content:center;}\n"
                    ".card{background:#fff;border-radius:20px;padding:48px 40px;text-align:center;"
                    "box-shadow:0 25px 50px rgba(0,0,0,0.3);max-width:440px;}\n"
                    ".icon{font-size:4rem;margin-bottom:16px;}\n"
                    "h1{font-size:1.6rem;color:#dc2626;margin-bottom:8px;}\n"
                    "p{color:#64748b;font-size:0.95rem;margin-bottom:24px;line-height:1.6;}\n"
                    ".links{display:flex;gap:10px;justify-content:center;flex-wrap:wrap;}\n"
                    ".links a{padding:10px 22px;background:#f1f5f9;color:#1e293b;text-decoration:none;"
                    "border-radius:10px;font-size:0.9rem;font-weight:500;border:1px solid #e2e8f0;transition:all .2s;}\n"
                    ".links a:hover{background:#e2e8f0;}\n"
                    ".links a.logout{padding:10px 22px;background:#fee2e2;color:#dc2626;text-decoration:none;"
                    "border-radius:10px;font-size:0.9rem;font-weight:500;border:1px solid #fecaca;}\n"
                    ".links a.logout:hover{background:#fecaca;}\n"
                    "</style></head><body>\n<div class=\"card\">\n"
                    "<div class=\"icon\">🔒</div>\n<h1>403 Forbidden</h1>\n"
                    "<p>You don't have permission to access this resource.<br>"
                    "Please contact the administrator if you believe this is an error.</p>\n"
                    "<div class=\"links\">\n"
                    "<a href=\"/\">← Back to Home</a>\n"
                    "<a href=\"/logout\" class=\"logout\">Logout</a>\n"
                    "</div>\n</div>\n</body></html>\n";
                return http_build_response(403, "FORBIDDEN", "text/html; charset=utf-8",
                                           page, (int)strlen(page), 0, output, size);
            }
        }

        /* Route matched — call the handler function */
        handler_fn fn = route_table_get_handler_fn(result.entry->handler);
        if (fn != NULL) {
            int resp_len = fn(req, body, body_size,
                              result.captured, output, size);
            if (resp_len >= 0) {
                return resp_len;
            }
            /* handler returned -1 (e.g. static file not found) */
            /* fall through to 404 */
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
                         "Content-Type: text/plain; charset=utf-8\r\n"
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

            /* ---- v1.7: extract Cookie header ---- */
            {
                const char *ck = strstr(recv_buf, "Cookie:");
                if (!ck) ck = strstr(recv_buf, "cookie:");
                if (ck) {
                    ck += 7; int k = 0;
                    while (*ck == ' ' || *ck == '\t') ck++;
                    while (ck[k] && ck[k] != '\r' && ck[k] != '\n'
                           && k < (int)sizeof(req.cookie) - 1)
                        { req.cookie[k] = ck[k]; k++; }
                    req.cookie[k] = '\0';
                }
            }

            /* ---- v1.6: extract Authorization header ---- */
            {
                fwrite(recv_buf, 1, n < 300 ? n : 300, stderr);
                fprintf(stderr, "]\n");
                const char *auth_start = strstr(recv_buf, "Authorization:");
                if (!auth_start) auth_start = strstr(recv_buf, "authorization:");
                if (auth_start) {
                    auth_start += 14;  /* skip "Authorization:" */
                    int k = 0;
                    while (*auth_start == ' ' || *auth_start == '\t') auth_start++;
                    while (auth_start[k] && auth_start[k] != '\r' && auth_start[k] != '\n'
                           && k < (int)sizeof(req.authorization) - 1) {
                        req.authorization[k] = auth_start[k];
                        k++;
                    }
                    req.authorization[k] = '\0';
                    { char *e = req.authorization + strlen(req.authorization) - 1;
                      while (e >= req.authorization && (*e == ' ' || *e == '\t'))
                          *e-- = '\0'; }
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
                             "Content-Type: text/plain; charset=utf-8\r\n"
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
