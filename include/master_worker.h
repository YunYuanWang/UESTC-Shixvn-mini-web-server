#ifndef MASTER_WORKER_H
#define MASTER_WORKER_H

#include "config.h"

/*
 * master_worker.h — Nginx-style master-worker process management (v1.0)
 *
 * Architecture:
 *   - Master process reads config, loads user data + BST index, creates
 *     a listen socket, forks N worker processes, then waits for signals.
 *   - Each worker inherits the listen socket and runs an independent epoll
 *     event loop (epoll_server_worker_run).
 *   - On SIGINT the master sends SIGTERM to all workers, waits up to
 *     worker_shutdown_timeout_ms, then escalates to SIGKILL.
 *
 * Usage:
 *   ./mini_web_server master conf/server.conf
 */

#define MASTER_MAX_WORKERS  128

/*
 * Run the master-worker server.
 * Returns 0 on clean shutdown.
 * Requires: log_init() already called, user_store already loaded.
 */
int master_worker_run(const server_config_t *config);

#endif /* MASTER_WORKER_H */
