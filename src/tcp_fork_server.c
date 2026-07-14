/*
 * tcp_fork_server.c — multi-process TCP/HTTP server
 *
 * Parent process creates a listening socket and loops on accept().
 * Each client connection is handled by a forked child process, allowing
 * multiple clients to be served concurrently.
 *
 * Signal handling:
 *   - SIGCHLD: handler calls waitpid(-1, &stat, WNOHANG) in a loop
 *              to reap all zombie children
 *   - SIGPIPE: ignored (SIG_IGN) so send() returns -1/EPIPE instead
 *              of killing the process when client disconnects early
 *   - accept() returns EINTR when interrupted by SIGCHLD; we continue
 *
 * The server exits after serving MAX_CLIENTS connections (for automated
 * testing with concurrent curl requests).
 */

#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/tcp_fork_server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- graceful shutdown via SIGINT ---- */
static volatile sig_atomic_t g_shutdown = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ================================================================
 *  signal handlers
 * ================================================================ */

/*
 * SIGCHLD handler — reap all exited children to avoid zombies.
 * Uses waitpid() in a loop with WNOHANG because multiple children
 * may have exited before the handler runs (signals are not queued).
 */
static void sigchld_handler(int sig) {
    int stat;
    pid_t pid;
    (void)sig;

    while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[ForkServer] reaped child PID %d (exit status %d)",
                 (int)pid, WEXITSTATUS(stat));
        log_info(msg);
    }
}

/* ================================================================
 *  tcp_fork_server_run
 * ================================================================ */
int tcp_fork_server_run(void) {
    int listen_fd;
    struct sockaddr_in server_addr;
    int optval;
    int clients_served = 0;
    char msg[256];

    /* ---- install signal handlers ---- */
    {
        struct sigaction sa;

        /* SIGINT: graceful shutdown (no SA_RESTART, accept returns EINTR) */
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);

        /* SIGCHLD: reap children, no SA_RESTART (so accept returns EINTR) */
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;  /* no SA_RESTART */
        sigaction(SIGCHLD, &sa, NULL);

        /* SIGPIPE: ignore so send() returns EPIPE instead of killing us */
        signal(SIGPIPE, SIG_IGN);
    }

    /* ---- startup banner ---- */
    log_info("========================================");
    {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "  ForkServer PID: %d  (multi-process mode)", (int)getpid());
        log_info(buf);
    }
    snprintf(msg, sizeof(msg),
             "  max_clients: %d", MAX_CLIENTS);
    log_info(msg);
    log_info("========================================");

    /* ---- create socket ---- */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("[ForkServer] socket() failed");
        return -1;
    }
    log_info("[ForkServer] socket created");

    /* ---- SO_REUSEADDR ---- */
    optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        log_error("[ForkServer] setsockopt(SO_REUSEADDR) failed");
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
        log_error("[ForkServer] bind(127.0.0.1:8080) failed");
        fprintf(stderr, "ERROR: bind(127.0.0.1:8080) failed — "
                "port may already be in use\n");
        close(listen_fd);
        return -1;
    }

    log_info("[ForkServer] listening on 127.0.0.1:8080");
    printf("ForkServer listening on http://127.0.0.1:8080  "
           "(max %d clients)\n", MAX_CLIENTS);

    /* ---- listen ---- */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_error("[ForkServer] listen() failed");
        close(listen_fd);
        return -1;
    }

    /* ================================================================
     *  main accept loop — fork per connection
     * ================================================================ */
    while (clients_served < MAX_CLIENTS && !g_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd;
        pid_t pid;

        conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr,
                         &client_len);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                /* interrupted by SIGCHLD or SIGINT.
                 * If shutting down, exit the loop.
                 * Otherwise just continue waiting. */
                if (g_shutdown) {
                    break;
                }
                continue;
            }
            log_error("[ForkServer] accept() failed");
            break;
        }

        /* ---- log client address ---- */
        {
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr,
                      client_ip, sizeof(client_ip));
            snprintf(msg, sizeof(msg),
                     "[ForkServer] [#%d] accepted connection from %s:%d",
                     clients_served + 1,
                     client_ip, ntohs(client_addr.sin_port));
            log_info(msg);
        }

        /* ---- fork child to handle this client ---- */
        pid = fork();
        if (pid < 0) {
            log_error("[ForkServer] fork() failed");
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            /* ================================================
             *  CHILD PROCESS
             * ================================================ */

            /* close the listening socket inherited from parent */
            close(listen_fd);

            /* handle the HTTP request */
            request_handler_handle_connection(conn_fd);

            /* close the client connection */
            close(conn_fd);

            /* _exit() — NOT exit() — avoids double-flush of stdio
             * buffers and atexit handlers inherited from parent */
            _exit(0);
        }

        /* ================================================
         *  PARENT PROCESS
         * ================================================ */

        /* parent closes its copy of the connected socket;
         * the child has its own copy and will close it when done */
        close(conn_fd);

        clients_served++;

        snprintf(msg, sizeof(msg),
                 "[ForkServer] child PID %d spawned for client #%d",
                 (int)pid, clients_served);
        log_info(msg);

        printf("[#%d] child PID %d handling request\n",
               clients_served, (int)pid);
    }

    /* ---- wait a moment for any remaining children to finish ---- */
    {
        int stat;
        /* brief grace period for children still sending responses */
        usleep(500000);  /* 500 ms */

        /* final sweep — reap any children that haven't been reaped yet */
        while (waitpid(-1, &stat, WNOHANG) > 0) {
            /* reaped */
        }
    }

    /* ---- cleanup ---- */
    close(listen_fd);

    snprintf(msg, sizeof(msg),
             "[ForkServer] server shutdown — served %d client(s)%s",
             clients_served,
             g_shutdown ? " (SIGINT)" : "");
    log_info(msg);
    printf("ForkServer: served %d clients, exiting%s.\n",
           clients_served,
           g_shutdown ? " (Ctrl-C)" : "");

    return 0;
}
