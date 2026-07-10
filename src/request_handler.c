#include "../include/request_handler.h"
#include "../include/http_response.h"
#include "../include/user_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    /* ---- GET /user/<name> (simple find, must be after /users/* routes) ---- */
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
