#ifndef TCP_THREAD_SERVER_H
#define TCP_THREAD_SERVER_H

/*
 * Run a multi-threaded TCP/HTTP server using thread-per-connection:
 *   1. Create a socket and bind to the given host:port
 *   2. Listen for incoming connections
 *   3. For each client, create a detached thread to handle the request
 *   4. Runs until SIGINT (Ctrl-C) is received
 *
 * This is the thread-based equivalent of tcp_fork_server (one thread
 * per connection vs one process per connection).  Threads share the
 * parent's address space, so memory overhead is lower than fork mode,
 * but there is no worker reuse (unlike the thread pool).
 *
 * Signal handling:
 *   - SIGINT:  graceful shutdown (sets flag, accept returns EINTR)
 *   - SIGPIPE: ignored so send() returns EPIPE instead of killing thread
 *   - accept() handles EINTR by continuing
 *
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */

#define THREAD_MAX_CONNS 10000

int tcp_thread_server_run(const char *host, int port);

#endif
