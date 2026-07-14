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
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- graceful shutdown via SIGINT ---- */
static volatile sig_atomic_t g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/*
 * Handle a single HTTP request on conn_fd.
 * Delegates to the shared request_handler_handle_connection().
 * Returns 0 on success, -1 on client error (caller should close conn_fd).
 */
static int handle_one_request(int conn_fd, int req_num) {
    int ret;

    printf("[#%d] ", req_num);
    fflush(stdout);

    ret = request_handler_handle_connection(conn_fd);

    return ret;
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
