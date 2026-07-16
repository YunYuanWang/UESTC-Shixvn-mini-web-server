#ifndef TCP_POOL_SERVER_H
#define TCP_POOL_SERVER_H

#define CORE_POOL_SIZE    2
#define MAX_POOL_SIZE     8
#define QUEUE_CAPACITY    128

/*
 * Run a multi-threaded TCP/HTTP server backed by a dynamic thread pool.
 *
 * Architecture:
 *   - Main thread creates a listening socket on the given host:port.
 *   - A thread pool with CORE_POOL_SIZE (2) workers is started; core
 *     workers block indefinitely on an empty work queue.
 *   - Main thread loops on accept().  Each accepted client_fd is
 *     enqueued as a task into the work queue.
 *   - Worker threads dequeue tasks, call the existing HTTP request
 *     handler, and close the client fd when done.
 *   - When all workers are busy and the queue is non-empty, the pool
 *     automatically scales up by spawning extra workers (up to
 *     MAX_POOL_SIZE).
 *   - Extra workers that are idle for IDLE_TIMEOUT_MS (1 s) exit
 *     automatically, shrinking the pool back toward CORE_POOL_SIZE.
 *   - Runs until SIGINT (Ctrl-C) is received, then shuts down the pool,
 *     wakes all workers, and waits for them to exit.
 *
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int tcp_pool_server_run(const char *host, int port);

#endif /* TCP_POOL_SERVER_H */
