#ifndef TCP_POOL_SERVER_H
#define TCP_POOL_SERVER_H

#define MAX_CLIENTS     5
#define NUM_WORKERS     4
#define QUEUE_CAPACITY  64

/*
 * Run a multi-threaded TCP/HTTP server backed by a thread pool.
 *
 * Architecture:
 *   - Main thread reads conf/server.conf, creates a listening socket
 *     on 127.0.0.1:8080.
 *   - A fixed-size thread pool (NUM_WORKERS) is started; all workers
 *     block on an empty work queue.
 *   - Main thread loops on accept().  Each accepted client_fd is
 *     enqueued as a task into the work queue.
 *   - Worker threads dequeue tasks, call the existing HTTP request
 *     handler, and close the client fd when done.
 *   - After MAX_CLIENTS connections have been accepted, the main
 *     thread stops accepting, shuts down the pool, wakes all workers,
 *     and waits for them to exit.
 *
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int tcp_pool_server_run(void);

#endif /* TCP_POOL_SERVER_H */
