/*
 * tcp_thread_server.c — multi-threaded TCP/HTTP server (thread-per-connection)
 *
 * Main thread creates a listening socket and loops on accept().
 * Each client connection is handled by a detached worker thread, allowing
 * multiple clients to be served concurrently without a thread pool.
 *
 * Contrast with other modes:
 *   - fork mode:  one process per connection (heavy, isolated)
 *   - pool mode:  fixed thread pool with task queue (worker reuse)
 *   - thread mode: one thread per connection (lighter than fork,
 *     heavier than pool; no worker reuse)
 *   - select mode: single-threaded I/O multiplexing (lightest)
 */

#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/tcp_thread_server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- graceful shutdown via SIGINT ---- */
static volatile sig_atomic_t g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ---- per-connection thread argument ---- */
typedef struct {
    int conn_fd;
    int thread_num;
} thread_arg_t;

/* ---- worker thread function ---- */
static void *worker_thread(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int conn_fd    = ta->conn_fd;
    int thread_num = ta->thread_num;

    /* set worker label for logging */
    request_handler_set_worker_label(thread_num);

    /* handle the HTTP request */
    request_handler_handle_connection(conn_fd);

    /* ensure orderly TCP shutdown */
    shutdown(conn_fd, SHUT_RDWR);
    close(conn_fd);

    free(ta);
    return NULL;
}

/* ================================================================
 *  tcp_thread_server_run
 * ================================================================ */
int tcp_thread_server_run(const char *host, int port) {
    int                listen_fd;
    struct sockaddr_in server_addr;
    int                optval;
    int                clients_served = 0;
    char               msg[256];

    /* ---- install signal handlers ---- */
    {
        struct sigaction sa;

        /* SIGINT: graceful shutdown (no SA_RESTART, accept returns EINTR) */
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);

        /* SIGPIPE: ignore so send() returns EPIPE instead of killing thread */
        signal(SIGPIPE, SIG_IGN);
    }

    /* ---- startup banner ---- */
    log_info("========================================");
    {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "  ThreadServer PID: %d  (thread-per-connection mode)",
                 (int)getpid());
        log_info(buf);
    }
    snprintf(msg, sizeof(msg),
             "  listening on %s:%d  (Ctrl-C to stop)", host, port);
    log_info(msg);
    log_info("========================================");

    /* ---- create socket ---- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("[ThreadServer] socket() failed");
        return -1;
    }
    log_info("[ThreadServer] socket created");

    /* ---- SO_REUSEADDR ---- */
    optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        log_error("[ThreadServer] setsockopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return -1;
    }

    /* ---- bind ---- */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
        snprintf(msg, sizeof(msg),
                 "[ThreadServer] invalid address: %s", host);
        log_error(msg);
        fprintf(stderr, "ERROR: invalid address '%s'\n", host);
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        snprintf(msg, sizeof(msg),
                 "[ThreadServer] bind(%s:%d) failed", host, port);
        log_error(msg);
        fprintf(stderr, "ERROR: bind(%s:%d) failed — "
                "port may already be in use\n", host, port);
        close(listen_fd);
        return -1;
    }

    snprintf(msg, sizeof(msg),
             "[ThreadServer] listening on %s:%d", host, port);
    log_info(msg);
    printf("ThreadServer listening on http://%s:%d  (Ctrl-C to stop)\n",
           host, port);

    /* ---- listen ---- */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_error("[ThreadServer] listen() failed");
        close(listen_fd);
        return -1;
    }

    /* ================================================================
     *  main accept loop — pthread_create per connection
     * ================================================================ */
    while (!g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t         client_len = sizeof(client_addr);
        int               conn_fd;
        pthread_t         tid;
        thread_arg_t     *ta;

        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
                         &client_len);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                if (g_shutdown) break;
                continue;
            }
            log_error("[ThreadServer] accept() failed");
            break;
        }

        /* ---- log client address ---- */
        {
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr,
                      client_ip, sizeof(client_ip));
            snprintf(msg, sizeof(msg),
                     "[ThreadServer] [#%d] accepted connection from %s:%d",
                     clients_served + 1,
                     client_ip, ntohs(client_addr.sin_port));
            log_info(msg);
        }

        /* ---- create worker thread ---- */
        ta = (thread_arg_t *)malloc(sizeof(thread_arg_t));
        if (ta == NULL) {
            log_error("[ThreadServer] malloc(thread_arg) failed");
            close(conn_fd);
            continue;
        }
        ta->conn_fd    = conn_fd;
        ta->thread_num = clients_served + 1;

        if (pthread_create(&tid, NULL, worker_thread, ta) != 0) {
            log_error("[ThreadServer] pthread_create() failed");
            free(ta);
            close(conn_fd);
            continue;
        }
        pthread_detach(tid);  /* fire-and-forget: thread cleans up itself */

        clients_served++;

        snprintf(msg, sizeof(msg),
                 "[ThreadServer] thread spawned for client #%d",
                 clients_served);
        log_info(msg);

        printf("[#%d] thread spawned\n", clients_served);
    }

    /* ---- wait a moment for remaining threads to finish ---- */
    usleep(500000);  /* 500 ms grace period */

    /* ---- cleanup ---- */
    close(listen_fd);

    snprintf(msg, sizeof(msg),
             "[ThreadServer] server shutdown — served %d client(s)%s",
             clients_served,
             g_shutdown ? " (SIGINT)" : "");
    log_info(msg);
    printf("ThreadServer: served %d clients, exiting%s.\n",
           clients_served,
           g_shutdown ? " (Ctrl-C)" : "");

    return 0;
}
