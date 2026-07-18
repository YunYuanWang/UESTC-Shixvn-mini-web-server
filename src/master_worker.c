/*
 * master_worker.c — Nginx-style master-worker process management (v1.0)
 *
 * Architecture:
 *   Master process:
 *     1. Creates listen socket (socket/bind/listen)
 *     2. Loads user data + BST index (done by main.c before calling us)
 *     3. Forks N worker processes
 *     4. Waits for SIGINT (or worker death)
 *     5. On SIGINT: sends SIGTERM to all workers
 *     6. Waits with timeout for workers to exit
 *     7. SIGKILL any stragglers, then close listen socket
 *
 *   Worker processes (after fork):
 *     1. Reopen log file (fresh independent FILE*)
 *     2. Run epoll event loop on inherited listen_fd
 *     3. _exit(0) when done
 */

#include "../include/master_worker.h"
#include "../include/epoll_server.h"
#include "../include/log.h"
#include "../include/user_store.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- master state ---- */
static volatile sig_atomic_t g_master_shutdown = 0;
static pid_t g_workers[MASTER_MAX_WORKERS];
static int    g_num_workers = 0;

/* ---- signal handlers ---- */
static void master_sigint_handler(int sig) {
    (void)sig;
    g_master_shutdown = 1;
}

static void master_sigchld_handler(int sig) {
    int status;
    pid_t wpid;
    (void)sig;

    /* reap all exited children */
    while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
        char msg[128];
        if (WIFEXITED(status)) {
            snprintf(msg, sizeof(msg),
                     "[Master] worker PID %d exited (status %d)",
                     (int)wpid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            snprintf(msg, sizeof(msg),
                     "[Master] worker PID %d killed by signal %d",
                     (int)wpid, WTERMSIG(status));
        } else {
            snprintf(msg, sizeof(msg),
                     "[Master] worker PID %d exited", (int)wpid);
        }
        log_info(msg);

        /* mark this worker as dead */
        for (int i = 0; i < g_num_workers; i++) {
            if (g_workers[i] == wpid) {
                g_workers[i] = 0;
                break;
            }
        }
    }
}

/* ---- internal helpers ---- */

/*
 * Create, bind, and listen on a TCP socket.
 * Returns the fd on success, -1 on error.
 */
static int master_create_listen_socket(const char *host, int port) {
    int listen_fd;
    struct sockaddr_in server_addr;
    int optval;
    char msg[128];

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("[Master] socket() failed");
        return -1;
    }

    optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0) {
        log_error("[Master] setsockopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
        snprintf(msg, sizeof(msg),
                 "[Master] invalid address: %s", host);
        log_error(msg);
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        snprintf(msg, sizeof(msg),
                 "[Master] bind(%s:%d) failed — port may be in use",
                 host, port);
        log_error(msg);
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_error("[Master] listen() failed");
        close(listen_fd);
        return -1;
    }

    snprintf(msg, sizeof(msg),
             "[Master] listening on %s:%d (fd=%d)", host, port, listen_fd);
    log_info(msg);

    return listen_fd;
}

/*
 * Send a signal to all alive workers.
 */
static void master_kill_workers(int signum) {
    const char *signame = (signum == SIGTERM) ? "SIGTERM" :
                          (signum == SIGKILL) ? "SIGKILL" : "SIGNAL";
    for (int i = 0; i < g_num_workers; i++) {
        if (g_workers[i] > 0) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "[Master] sending %s to worker PID %d",
                     signame, (int)g_workers[i]);
            log_info(msg);
            kill(g_workers[i], signum);
        }
    }
}

/*
 * Wait for all workers to exit, with a timeout.
 * Returns the number of workers still alive after the timeout.
 */
static int master_wait_for_workers(int timeout_ms) {
    int remaining = 0;
    struct timeval start, now;

    /* count currently alive workers */
    for (int i = 0; i < g_num_workers; i++) {
        if (g_workers[i] > 0) remaining++;
    }

    if (remaining == 0) return 0;

    gettimeofday(&start, NULL);

    while (remaining > 0) {
        int status;
        pid_t wpid;
        long elapsed_ms;

        wpid = waitpid(-1, &status, WNOHANG);
        if (wpid > 0) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "[Master] reaped worker PID %d", (int)wpid);
            log_info(msg);

            for (int i = 0; i < g_num_workers; i++) {
                if (g_workers[i] == wpid) {
                    g_workers[i] = 0;
                    break;
                }
            }
            remaining--;
            continue;
        } else if (wpid == 0) {
            /* no child exited yet — check timeout */
            gettimeofday(&now, NULL);
            elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                         (now.tv_usec - start.tv_usec) / 1000;
            if (elapsed_ms >= timeout_ms && timeout_ms > 0) {
                break;
            }
            usleep(50000);  /* 50 ms polling interval */
        } else {
            /* error or no children */
            if (errno == ECHILD) {
                remaining = 0;
            }
            break;
        }
    }

    /* recount remaining */
    remaining = 0;
    for (int i = 0; i < g_num_workers; i++) {
        if (g_workers[i] > 0) remaining++;
    }
    return remaining;
}

/* ================================================================
 *  master_worker_run — main entry point
 * ================================================================ */
int master_worker_run(const server_config_t *config) {
    int listen_fd;
    int num_workers;
    int timeout_ms;
    const char *log_path;
    int ret = -1;

    num_workers = config->worker_processes;
    if (num_workers <= 0) num_workers = 2;
    if (num_workers > MASTER_MAX_WORKERS) num_workers = MASTER_MAX_WORKERS;

    timeout_ms = config->worker_shutdown_timeout_ms;
    if (timeout_ms <= 0) timeout_ms = 3000;

    log_path = config->log_path;

    /* ---- Phase 1: install master signal handlers ---- */
    {
        struct sigaction sa;

        /* SIGINT: graceful shutdown */
        sa.sa_handler = master_sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);

        /* SIGCHLD: reap dead children */
        sa.sa_handler = master_sigchld_handler;
        sa.sa_flags = SA_RESTART;  /* restart interrupted syscalls */
        sigaction(SIGCHLD, &sa, NULL);

        /* SIGPIPE: ignore */
        signal(SIGPIPE, SIG_IGN);
    }

    /* ---- Phase 1b: startup banner ---- */
    log_info("========================================");
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "  Master PID: %d  starting %d worker(s)  "
                 "shutdown_timeout: %d ms",
                 (int)getpid(), num_workers, timeout_ms);
        log_info(buf);
    }
    log_info("========================================");

    /* ---- Phase 2: create listen socket ---- */
    listen_fd = master_create_listen_socket(config->host, config->port);
    if (listen_fd < 0) {
        log_error("[Master] failed to create listen socket");
        return -1;
    }

    printf("[Master] listening on http://%s:%d  "
           "(%d worker(s), Ctrl-C to stop)\n",
           config->host, config->port, num_workers);
    fflush(stdout);

    /* ---- Phase 3: fork workers ---- */
    g_num_workers = 0;
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            log_error("[Master] fork() failed");
            /* kill already-forked workers */
            master_kill_workers(SIGTERM);
            usleep(100000);
            master_kill_workers(SIGKILL);
            goto cleanup;
        }

        if (pid == 0) {
            /* ============================================
             *  CHILD: worker process
             * ============================================ */

            /*
             * Reopen log with a fresh independent FILE*.
             * This avoids cross-process buffer corruption from the
             * inherited FILE* state.  O_APPEND ensures atomic appends.
             */
            log_reopen(log_path);

            /*
             * Reset signal handlers inherited from master.
             * SIGINT/SIGTERM are handled by epoll_server_worker_run.
             * SIGCHLD is irrelevant for workers.
             */
            signal(SIGCHLD, SIG_DFL);

            /* Log worker startup with PID for verification */
            {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "[Worker] PID %d started, listen_fd=%d",
                         (int)getpid(), listen_fd);
                log_info(msg);
            }

            /*
             * Run the epoll event loop on the inherited listen socket.
             * This function returns when g_shutdown is set (SIGINT/SIGTERM).
             */
            epoll_server_worker_run(listen_fd, i + 1);  /* worker IDs are 1-based */

            /* clean exit */
            {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "[Worker] PID %d shutting down", (int)getpid());
                log_info(msg);
            }
            log_close();
            _exit(0);
        }

        /* ============================================
         *  PARENT: track worker PID
         * ============================================ */
        g_workers[g_num_workers++] = pid;
        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "[Master] forked worker %d, PID %d", i + 1, (int)pid);
            log_info(msg);
        }
    }

    printf("[Master] %d worker(s) started (PIDs:",
           g_num_workers);
    for (int i = 0; i < g_num_workers; i++) {
        printf(" %d", (int)g_workers[i]);
    }
    printf(")\n");
    fflush(stdout);

    /* ---- Phase 4: master wait loop ---- */
    {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "[Master] entering wait loop (%d workers)", g_num_workers);
        log_info(msg);
    }

    while (!g_master_shutdown) {
        pause();  /* sleep until a signal arrives */
        /*
         * pause() returns on any signal.  If it's SIGINT,
         * g_master_shutdown is set and we exit the loop.
         * If it's SIGCHLD, the handler reaps and we loop.
         */
    }

    /* ---- Phase 5: graceful shutdown ---- */
    log_info("[Master] shutting down...");

    /* 5a: close listen socket so no new connections are accepted */
    close(listen_fd);
    log_info("[Master] listen socket closed");

    /* 5b: send SIGTERM to all workers */
    master_kill_workers(SIGTERM);

    /* 5c: wait for workers with timeout */
    {
        int remaining;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[Master] waiting up to %d ms for workers to exit",
                 timeout_ms);
        log_info(msg);

        remaining = master_wait_for_workers(timeout_ms);

        if (remaining > 0) {
            /* 5d: timeout — force kill remaining workers */
            snprintf(msg, sizeof(msg),
                     "[Master] %d worker(s) still alive after timeout, "
                     "sending SIGKILL", remaining);
            log_info(msg);

            master_kill_workers(SIGKILL);
            usleep(100000);  /* 100 ms grace for SIGKILL */
            master_wait_for_workers(1000);  /* final sweep, 1 s max */
        }
    }

    log_info("[Master] all workers stopped");
    log_info("========================================");
    {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "  Master PID: %d  shutdown complete", (int)getpid());
        log_info(buf);
    }
    log_info("========================================");
    printf("[Master] shutdown complete.\n");
    fflush(stdout);

    ret = 0;

cleanup:
    /* close listen_fd if we bailed out early */
    if (ret == -1 && listen_fd >= 0) {
        close(listen_fd);
    }
    return ret;
}
