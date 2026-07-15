/*
 * tcp_pool_server.c — multi-threaded TCP/HTTP server with thread pool
 *
 * Main thread:
 *   1. Creates a listening socket on 127.0.0.1:8080
 *   2. Starts a fixed-size thread pool (all workers block on empty queue)
 *   3. Loops on accept(), enqueuing each client_fd into the work queue
 *   4. After POOL_MAX_CLIENTS connections, shuts down the pool and exits
 *
 * Worker threads:
 *   - Dequeue a client_fd from the queue
 *   - Call request_handler_handle_connection() to process the HTTP request
 *   - Close the client_fd
 *   - Loop until shutdown is signaled and the queue is drained
 *
 * Signal handling:
 *   - SIGINT:  graceful shutdown (sets flag, accept returns EINTR)
 *   - SIGPIPE: ignored (send() returns EPIPE instead of killing thread)
 */

#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/tcp_pool_server.h"
#include "../include/thread_pool.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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

/* ================================================================
 *  tcp_pool_server_run
 * ================================================================ */
int tcp_pool_server_run(void) {
    int listen_fd;
    struct sockaddr_in server_addr;
    int optval;
    int clients_served = 0;
    thread_pool_t *pool = NULL;
    char msg[256];

    /* ---- install signal handlers ---- */
    {
        struct sigaction sa;

        /* SIGINT: graceful shutdown (no SA_RESTART, accept returns EINTR) */
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
                 "  PoolServer PID: %d  (thread-pool mode)", (int)getpid());
        log_info(buf);
    }
    snprintf(msg, sizeof(msg),
             "  max_clients: %d  core_workers: %d  max_workers: %d",
             POOL_MAX_CLIENTS, CORE_POOL_SIZE, MAX_POOL_SIZE);
    log_info(msg);
    log_info("========================================");

    /* ---- create socket ---- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("[PoolServer] socket() failed");
        return -1;
    }
    log_info("[PoolServer] socket created");

    /* ---- SO_REUSEADDR ---- */
    optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        log_error("[PoolServer] setsockopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return -1;
    }

    /* ---- bind ---- */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        log_error("[PoolServer] bind(127.0.0.1:8080) failed");
        fprintf(stderr, "ERROR: bind(127.0.0.1:8080) failed — "
                "port may already be in use\n");
        close(listen_fd);
        return -1;
    }

    log_info("[PoolServer] listening on 127.0.0.1:8080");
    printf("PoolServer listening on http://127.0.0.1:8080  "
           "(max %d clients, core=%d max=%d workers)\n",
           POOL_MAX_CLIENTS, CORE_POOL_SIZE, MAX_POOL_SIZE);

    /* ---- listen ---- */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_error("[PoolServer] listen() failed");
        close(listen_fd);
        return -1;
    }

    /* ---- create thread pool ---- */
    pool = thread_pool_create(CORE_POOL_SIZE, MAX_POOL_SIZE, QUEUE_CAPACITY);
    if (pool == NULL) {
        log_error("[PoolServer] thread_pool_create failed");
        close(listen_fd);
        return -1;
    }

    /* ================================================================
     *  main accept loop — enqueue client fds into work queue
     * ================================================================ */
    while (clients_served < POOL_MAX_CLIENTS && !g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd;

        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
                         &client_len);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                /* interrupted by signal — check if we should shut down */
                if (g_shutdown) {
                    break;
                }
                continue;
            }
            log_error("[PoolServer] accept() failed");
            break;
        }

        /* ---- log client address ---- */
        {
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr,
                      client_ip, sizeof(client_ip));
            snprintf(msg, sizeof(msg),
                     "[PoolServer] [#%d] accepted connection from %s:%d",
                     clients_served + 1,
                     client_ip, ntohs(client_addr.sin_port));
            log_info(msg);
        }

        /* ---- enqueue the client fd ---- */
        if (thread_pool_add_task(pool, conn_fd) != 0) {
            log_error("[PoolServer] failed to enqueue task (pool shutting down)");
            close(conn_fd);
            break;
        }

        clients_served++;

        snprintf(msg, sizeof(msg),
                 "[PoolServer] task #%d enqueued (fd=%d)",
                 clients_served, conn_fd);
        log_info(msg);

        printf("[#%d] connection enqueued (fd=%d)\n",
               clients_served, conn_fd);
    }

    /* ---- close the listening socket (no more accepts) ---- */
    close(listen_fd);
    log_info("[PoolServer] listening socket closed");

    /* ================================================================
     *  shutdown — stop accepting, drain the queue, join workers
     * ================================================================ */

    log_info("[PoolServer] shutting down thread pool...");

    /* signal the pool to drain and stop */
    thread_pool_shutdown(pool);

    /*
     * Wait for all workers to complete in-flight tasks and exit.
     * thread_pool_destroy joins all workers, snapshots final stats,
     * and then frees pool resources.
     */
    {
        int processed = 0;
        int errors    = 0;
        int peak      = thread_pool_get_peak_workers(pool);

        thread_pool_destroy(pool, &processed, &errors);

        /* ---- summary ---- */

        snprintf(msg, sizeof(msg),
                 "[PoolServer] server shutdown — "
                 "accepted %d client(s), processed %d, errors %d, "
                 "peak_workers %d%s",
                 clients_served, processed, errors, peak,
                 g_shutdown ? " (SIGINT)" : "");
        log_info(msg);

        printf("PoolServer: accepted %d clients, processed %d, errors %d, "
               "peak_workers %d, exiting%s.\n",
               clients_served, processed, errors, peak,
               g_shutdown ? " (Ctrl-C)" : "");
    }

    return 0;
}
