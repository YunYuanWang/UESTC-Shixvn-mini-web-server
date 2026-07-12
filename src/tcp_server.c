/*
 * tcp_server.c — multi-request TCP/HTTP server
 *
 * Creates a socket, binds to the configured address/port, listens for
 * incoming connections, and handles HTTP requests one at a time in a
 * loop until the server is shut down (Ctrl-C / SIGINT).
 */

#include "../include/config.h"
#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/tcp_server.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RECV_BUF_SIZE 8192
#define RESP_BUF_SIZE 16384

/* ---- graceful shutdown via SIGINT ---- */
static volatile sig_atomic_t g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

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
        while (end > body_start && (*(end - 1) == '\r' || *(end - 1) == '\n')) {
            end--;
            *end = '\0';
        }
    }

    if (body_start[0] == '\0') {
        return NULL;
    }

    return body_start;
}

/*
 * Extract the HTTP status code from a response buffer.
 */
static int extract_response_status(const char *resp) {
    if (resp == NULL) {
        return 0;
    }
    if (strncmp(resp, "HTTP/1.1 ", 9) == 0) {
        return atoi(resp + 9);
    }
    return 0;
}

/*
 * Handle a single HTTP request on conn_fd:
 *   recv → parse → route → build response → send
 * Returns 0 on success, -1 on client error (caller should close conn_fd).
 */
static int handle_one_request(int conn_fd, int req_num) {
    char recv_buf[RECV_BUF_SIZE];
    char resp_buf[RESP_BUF_SIZE];
    char msg[512];
    ssize_t n;

    /* ---- read HTTP request ---- */
    n = recv(conn_fd, recv_buf, sizeof(recv_buf) - 1, 0);
    if (n <= 0) {
        log_error("[TCPServer] recv() failed or empty request");
        return -1;
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
            log_error("[TCPServer] malformed HTTP request line");
            snprintf(resp_buf, sizeof(resp_buf),
                     "HTTP/1.1 400 BAD REQUEST\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: 17\r\n"
                     "\r\n"
                     "400 Bad Request\n");
            send(conn_fd, resp_buf, strlen(resp_buf), 0);
            log_info("[TCPServer] response: (malformed) → 400 BAD REQUEST");
            return -1;
        }

        snprintf(msg, sizeof(msg),
                 "[TCPServer] request: %s %s", method, path);
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
                     "[TCPServer] body: %s", body);
            log_info(msg);
        }

        /* generate HTTP response */
        if (request_handler_process_http(&req, resp_buf,
                                          sizeof(resp_buf)) < 0) {
            log_error("[TCPServer] request_handler_process_http failed");
            snprintf(resp_buf, sizeof(resp_buf),
                     "HTTP/1.1 500 INTERNAL SERVER ERROR\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: 25\r\n"
                     "\r\n"
                     "500 Internal Server Error\n");
        }

        /* log response status */
        resp_status = extract_response_status(resp_buf);
        if (resp_status > 0) {
            snprintf(msg, sizeof(msg),
                     "[TCPServer] response: %s %s → %d",
                     method, path, resp_status);
        } else {
            snprintf(msg, sizeof(msg),
                     "[TCPServer] response: %s %s → (unknown status)",
                     method, path);
        }
        log_info(msg);

        /* ---- send response ---- */
        if (send(conn_fd, resp_buf, strlen(resp_buf), 0) < 0) {
            log_error("[TCPServer] send() failed");
            return -1;
        }

        /* console summary */
        printf("[#%d] %s %s → %d\n",
               req_num, method, path, resp_status);
    }

    return 0;
}

/* ================================================================
 *  tcp_server_run — main loop
 * ================================================================ */
int tcp_server_run(const server_config_t *config) {
    int listen_fd;
    struct sockaddr_in server_addr;
    int optval;
    char msg[512];
    int request_count = 0;

    /* ---- install SIGINT handler (no SA_RESTART, so accept() returns EINTR) ---- */
    {
        struct sigaction sa;
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
    }

    /* ---- startup banner ---- */
    log_info("========================================");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "  Server PID: %d  (TCP mode)", (int)getpid());
        log_info(buf);
    }
    log_info("========================================");

    /* ---- create socket ---- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("[TCPServer] socket() failed");
        return -1;
    }
    log_info("[TCPServer] socket created");

    /* ---- SO_REUSEADDR ---- */
    optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        log_error("[TCPServer] setsockopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return -1;
    }

    /* ---- bind ---- */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config->port);
    server_addr.sin_addr.s_addr = inet_addr(config->host);

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        snprintf(msg, sizeof(msg),
                 "[TCPServer] bind(%s:%d) failed",
                 config->host, config->port);
        log_error(msg);
        close(listen_fd);
        return -1;
    }

    snprintf(msg, sizeof(msg),
             "[TCPServer] listening on %s:%d", config->host, config->port);
    log_info(msg);
    printf("Server listening on http://%s:%d  (Ctrl-C to stop)\n",
           config->host, config->port);

    /* ---- listen (larger backlog for multi-request) ---- */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_error("[TCPServer] listen() failed");
        close(listen_fd);
        return -1;
    }

    /* ================================================================
     *  main accept loop
     * ================================================================ */
    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd;

        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
                         &client_len);
        if (conn_fd < 0) {
            if (g_shutdown) {
                break;  /* interrupted by signal, normal exit */
            }
            log_error("[TCPServer] accept() failed");
            continue;
        }

        /* log client address */
        {
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr,
                      client_ip, sizeof(client_ip));
            snprintf(msg, sizeof(msg),
                     "[TCPServer] [#%d] accepted connection from %s:%d",
                     request_count + 1,
                     client_ip, ntohs(client_addr.sin_port));
            log_info(msg);
        }

        /* handle the request */
        if (handle_one_request(conn_fd, request_count + 1) == 0) {
            request_count++;
        }

        /* close client connection */
        close(conn_fd);
        log_info("[TCPServer] connection closed");
    }

    /* ---- cleanup ---- */
    close(listen_fd);

    snprintf(msg, sizeof(msg),
             "[TCPServer] server shutdown — handled %d request(s)",
             request_count);
    log_info(msg);

    return 0;
}
