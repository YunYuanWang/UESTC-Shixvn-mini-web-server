#ifndef SELECT_SERVER_H
#define SELECT_SERVER_H

/*
 * select_server.h — I/O multiplexing TCP/HTTP server using select()
 *
 * Architecture:
 *   - Single-threaded event loop using select() to monitor all active
 *     file descriptors (listen socket + client connections).
 *   - When the listen fd is ready, accept() a new connection and add
 *     its fd to the master fd_set.
 *   - When a client fd is ready, recv() the HTTP request, process it
 *     through the existing request_handler, send the response, and
 *     close the connection.
 *   - Supports up to FD_SETSIZE concurrent connections.
 *
 * Usage:
 *   ./mini_web_server select <ip> <port>
 *
 * Contrast with other modes:
 *   - fork mode:  one process per connection (heavy, isolated)
 *   - pool mode:  fixed thread pool (shared memory, medium overhead)
 *   - select mode: single-threaded I/O multiplexing (lightweight, no
 *     threading overhead, but one slow client can block others since
 *     request processing is synchronous in the event loop)
 */

#define SELECT_MAX_CONNS  1024

/*
 * Run a select-based TCP/HTTP server on the given host:port.
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int select_server_run(const char *host, int port);

#endif /* SELECT_SERVER_H */
