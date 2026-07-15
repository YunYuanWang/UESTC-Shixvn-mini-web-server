/*
 * select_server.c — I/O multiplexing TCP/HTTP server using select()
 *
 * Architecture:
 *   - Single-threaded event loop.
 *   - select() monitors the listen fd + all active client fds.
 *   - Listen fd ready  → accept(), add client fd to master fd_set.
 *   - Client fd ready  → recv() HTTP request, process, send response,
 *                         close fd, remove from master fd_set.
 *
 * Connection tracking:
 *   A fixed-size array of conn_t records each active connection's fd
 *   and receive buffer.  Free slots have fd == -1.
 *
 * Signal handling:
 *   - SIGINT:  sets g_shutdown flag; select() returns EINTR.
 *   - SIGPIPE: ignored (SIG_IGN).
 */

#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/select_server.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- per-connection state ---- */
#define RECV_BUF_SIZE  8192
#define RESP_BUF_SIZE 16384

typedef struct {
    int   fd;               /* -1 = slot free */
    char  recv_buf[RECV_BUF_SIZE];
    int   recv_len;
} conn_t;

/* ---- graceful shutdown via SIGINT ---- */
static volatile sig_atomic_t g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ================================================================
 *  select_server_run
 * ================================================================ */
int select_server_run(const char *host, int port) {
    int          listen_fd;
    struct sockaddr_in server_addr;
    int          optval;
    fd_set       master_set, read_set;
    int          max_fd;
    conn_t      *connections = NULL;
    int          clients_served = 0;
    int          ret = -1;
    char         msg[1024];

    /* ---- allocate connection array ---- */
    connections = (conn_t *)calloc(SELECT_MAX_CONNS, sizeof(conn_t));
    if (connections == NULL) {
        log_error("[SelectServer] failed to allocate connection array");
        return -1;
    }
    for (int i = 0; i < SELECT_MAX_CONNS; i++) {
        connections[i].fd = -1;
    }

    /* ---- install signal handlers ---- */
    {
        struct sigaction sa;
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);

        /* SIGPIPE: ignore so send() returns EPIPE instead of killing us */
        signal(SIGPIPE, SIG_IGN);
    }

    /* ---- startup banner ---- */
    log_info("========================================");
    {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "  SelectServer PID: %d  (select I/O multiplexing mode)",
                 (int)getpid());
        log_info(buf);
    }
    snprintf(msg, sizeof(msg),
             "  listening on %s:%d  max_connections: %d",
             host, port, SELECT_MAX_CONNS);
    log_info(msg);
    log_info("========================================");

    /* ---- create socket ---- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("[SelectServer] socket() failed");
        goto cleanup;
    }
    log_info("[SelectServer] socket created");

    /* ---- SO_REUSEADDR ---- */
    optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        log_error("[SelectServer] setsockopt(SO_REUSEADDR) failed");
        goto cleanup;
    }

    /* ---- bind ---- */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
        snprintf(msg, sizeof(msg),
                 "[SelectServer] invalid address: %s", host);
        log_error(msg);
        fprintf(stderr, "ERROR: invalid address '%s'\n", host);
        goto cleanup;
    }

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        snprintf(msg, sizeof(msg),
                 "[SelectServer] bind(%s:%d) failed", host, port);
        log_error(msg);
        fprintf(stderr, "ERROR: bind(%s:%d) failed — "
                "port may already be in use\n", host, port);
        goto cleanup;
    }

    snprintf(msg, sizeof(msg),
             "[SelectServer] listening on %s:%d", host, port);
    log_info(msg);
    printf("SelectServer listening on http://%s:%d  (Ctrl-C to stop)\n",
           host, port);

    /* ---- listen ---- */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_error("[SelectServer] listen() failed");
        goto cleanup;
    }

    /* ---- init fd_sets ---- */
    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    max_fd = listen_fd;

    /* ================================================================
     *  main event loop
     * ================================================================ */
    while (!g_shutdown) {
        int activity;

        read_set = master_set;

        /*
         * select() blocks until one or more fds are ready.
         * No timeout — wait indefinitely until activity or SIGINT.
         */
        activity = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (activity < 0) {
            if (errno == EINTR) {
                /* interrupted by SIGINT — exit loop */
                if (g_shutdown) break;
                continue;
            }
            log_error("[SelectServer] select() failed");
            break;
        }

        if (activity == 0) {
            continue;  /* timeout — should not happen (no timeout set) */
        }

        /* ---- scan all fds up to max_fd ---- */
        for (int fd = 0; fd <= max_fd; fd++) {
            if (!FD_ISSET(fd, &read_set)) {
                continue;
            }

            if (fd == listen_fd) {
                /* ============================================
                 *  new incoming connection
                 * ============================================ */
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int conn_fd;

                conn_fd = accept(listen_fd,
                                 (struct sockaddr *)&client_addr,
                                 &client_len);
                if (conn_fd < 0) {
                    if (errno == EINTR) continue;
                    log_error("[SelectServer] accept() failed");
                    continue;
                }

                /* find a free slot */
                {
                    int slot = -1;
                    for (int i = 0; i < SELECT_MAX_CONNS; i++) {
                        if (connections[i].fd == -1) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot == -1) {
                        log_error("[SelectServer] connection table full — "
                                  "rejecting new client");
                        close(conn_fd);
                        continue;
                    }

                    connections[slot].fd       = conn_fd;
                    connections[slot].recv_len = 0;
                    memset(connections[slot].recv_buf, 0, RECV_BUF_SIZE);

                    FD_SET(conn_fd, &master_set);
                    if (conn_fd > max_fd) {
                        max_fd = conn_fd;
                    }
                }

                /* log client address */
                {
                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr,
                              client_ip, sizeof(client_ip));
                    snprintf(msg, sizeof(msg),
                             "[SelectServer] [#%d] accepted connection "
                             "from %s:%d (fd=%d)",
                             clients_served + 1,
                             client_ip, ntohs(client_addr.sin_port),
                             conn_fd);
                    log_info(msg);
                }

                clients_served++;
                printf("[#%d] accepted (fd=%d)\n", clients_served, conn_fd);

            } else {
                /* ============================================
                 *  data from an existing client
                 * ============================================ */
                int slot = -1;
                for (int i = 0; i < SELECT_MAX_CONNS; i++) {
                    if (connections[i].fd == fd) {
                        slot = i;
                        break;
                    }
                }
                if (slot == -1) {
                    /* should not happen — fd in read_set but not in
                     * our connection table */
                    FD_CLR(fd, &master_set);
                    close(fd);
                    if (fd == max_fd) max_fd--;
                    continue;
                }

                {
                    conn_t *conn = &connections[slot];
                    ssize_t n;

                    n = recv(fd, conn->recv_buf + conn->recv_len,
                             RECV_BUF_SIZE - conn->recv_len - 1, 0);

                    if (n <= 0) {
                        /* client closed or error */
                        if (n < 0) {
                            snprintf(msg, sizeof(msg),
                                     "[SelectServer] recv() error on fd=%d",
                                     fd);
                            log_error(msg);
                        }
                        /* clean up this connection */
                        FD_CLR(fd, &master_set);
                        close(fd);
                        conn->fd = -1;
                        if (fd == max_fd) {
                            while (max_fd > listen_fd &&
                                   !FD_ISSET(max_fd, &master_set)) {
                                max_fd--;
                            }
                        }
                        continue;
                    }

                    conn->recv_len += (int)n;
                    conn->recv_buf[conn->recv_len] = '\0';

                    /*
                     * Check if we have a complete HTTP request.
                     * A minimal check: look for \r\n\r\n (header end)
                     * or just process on first recv (simple approach).
                     *
                     * For consistency with the other servers, we
                     * process whatever we get on the first recv
                     * (matching request_handler_handle_connection's
                     * single-recv behavior).
                     */
                    {
                        char resp_buf[RESP_BUF_SIZE];
                        char *method = NULL;
                        char *path   = NULL;
                        char line_copy[512];
                        request_t req;
                        char *body;
                        char *nl;
                        int resp_status;
                        ssize_t sent;

                        /* ---- parse HTTP request line ---- */
                        strncpy(line_copy, conn->recv_buf,
                                sizeof(line_copy) - 1);
                        line_copy[sizeof(line_copy) - 1] = '\0';
                        nl = strchr(line_copy, '\r');
                        if (nl != NULL) *nl = '\0';
                        nl = strchr(line_copy, '\n');
                        if (nl != NULL) *nl = '\0';

                        method = line_copy;
                        nl = strchr(line_copy, ' ');
                        if (nl == NULL) {
                            /* malformed — send 400 */
                            snprintf(resp_buf, sizeof(resp_buf),
                                     "HTTP/1.1 400 BAD REQUEST\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 17\r\n"
                                     "\r\n"
                                     "400 Bad Request\n");
                            send(fd, resp_buf, strlen(resp_buf), 0);
                            FD_CLR(fd, &master_set);
                            close(fd);
                            conn->fd = -1;
                            if (fd == max_fd) {
                                while (max_fd > listen_fd &&
                                       !FD_ISSET(max_fd, &master_set)) {
                                    max_fd--;
                                }
                            }
                            log_info("[SelectServer] response: "
                                     "(malformed) -> 400 BAD REQUEST");
                            continue;
                        }
                        *nl = '\0';
                        path = nl + 1;
                        while (*path == ' ') path++;

                        nl = strchr(path, ' ');
                        if (nl != NULL) *nl = '\0';

                        snprintf(msg, sizeof(msg),
                                 "[SelectServer] request: %s %s",
                                 method, path);
                        log_info(msg);

                        /* ---- fill request_t ---- */
                        memset(&req, 0, sizeof(req));
                        {
                            int i;
                            for (i = 0;
                                 method[i] != '\0' &&
                                 i < (int)sizeof(req.method) - 1;
                                 i++) {
                                req.method[i] = (char)toupper(
                                    (unsigned char)method[i]);
                            }
                            req.method[i] = '\0';
                        }
                        strncpy(req.path, path, sizeof(req.path) - 1);
                        req.path[sizeof(req.path) - 1] = '\0';

                        /* extract body (if present) */
                        {
                            char *body_start;
                            body_start = strstr(conn->recv_buf, "\r\n\r\n");
                            if (body_start != NULL) {
                                body_start += 4;
                                if (body_start < conn->recv_buf +
                                                   conn->recv_len &&
                                    body_start[0] != '\0') {
                                    strncpy(req.body, body_start,
                                            sizeof(req.body) - 1);
                                    req.body[sizeof(req.body) - 1] = '\0';
                                    /* strip trailing \r\n */
                                    {
                                        char *end = req.body +
                                                    strlen(req.body);
                                        while (end > req.body &&
                                               (*(end - 1) == '\r' ||
                                                *(end - 1) == '\n')) {
                                            end--;
                                            *end = '\0';
                                        }
                                    }
                                }
                            }
                        }

                        /* ---- generate HTTP response ---- */
                        if (request_handler_process_http(&req, resp_buf,
                                                         sizeof(resp_buf))
                            < 0) {
                            snprintf(resp_buf, sizeof(resp_buf),
                                     "HTTP/1.1 500 INTERNAL SERVER ERROR\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 25\r\n"
                                     "\r\n"
                                     "500 Internal Server Error\n");
                        }

                        /* extract and log response status */
                        if (strncmp(resp_buf, "HTTP/1.1 ", 9) == 0) {
                            resp_status = atoi(resp_buf + 9);
                            snprintf(msg, sizeof(msg),
                                     "[SelectServer] response: %s %s -> %d",
                                     method, path, resp_status);
                        } else {
                            resp_status = 0;
                            snprintf(msg, sizeof(msg),
                                     "[SelectServer] response: %s %s -> "
                                     "(unknown status)",
                                     method, path);
                        }
                        log_info(msg);

                        printf("%s %s -> %d\n", method, path, resp_status);

                        /* ---- send response ---- */
                        sent = send(fd, resp_buf, strlen(resp_buf), 0);
                        if (sent < 0) {
                            snprintf(msg, sizeof(msg),
                                     "[SelectServer] send() failed on fd=%d",
                                     fd);
                            log_error(msg);
                        }

                        /* ---- close connection ---- */
                        FD_CLR(fd, &master_set);
                        close(fd);
                        conn->fd = -1;
                        if (fd == max_fd) {
                            while (max_fd > listen_fd &&
                                   !FD_ISSET(max_fd, &master_set)) {
                                max_fd--;
                            }
                        }
                        log_info("[SelectServer] connection closed");
                    }
                }
            }
        }
    }

    /* ---- shutdown: close all remaining connections ---- */
    log_info("[SelectServer] shutting down...");

    for (int i = 0; i < SELECT_MAX_CONNS; i++) {
        if (connections[i].fd != -1) {
            FD_CLR(connections[i].fd, &master_set);
            close(connections[i].fd);
            connections[i].fd = -1;
        }
    }

    /* close listen socket */
    FD_CLR(listen_fd, &master_set);
    close(listen_fd);

    snprintf(msg, sizeof(msg),
             "[SelectServer] server shutdown — served %d client(s)%s",
             clients_served,
             g_shutdown ? " (SIGINT)" : "");
    log_info(msg);
    printf("SelectServer: served %d clients, exiting%s.\n",
           clients_served,
           g_shutdown ? " (Ctrl-C)" : "");

    ret = 0;

cleanup:
    free(connections);
    /* close listen_fd if we bailed out early (goto cleanup) */
    if (ret == -1 && listen_fd >= 0) {
        close(listen_fd);
    }
    return ret;
}
