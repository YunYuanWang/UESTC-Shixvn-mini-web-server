#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

/*
 * epoll_server.h — I/O multiplexing TCP/HTTP server using epoll()
 *
 * Architecture:
 *   - Single-threaded event loop using epoll_create1 / epoll_ctl /
 *     epoll_wait to monitor all active file descriptors (listen socket
 *     + client connections).
 *   - When the listen fd is ready (EPOLLIN), accept() a new connection
 *     and add its fd to the epoll interest list.
 *   - When a client fd is ready (EPOLLIN), recv() the HTTP request,
 *     process it through the existing request_handler, send the HTTP/1.1
 *     response, and close the connection (or keep-alive).
 *   - Level-triggered (LT) by default — same semantics as select().
 *   - Real-time stdout output: connection count, client address, and
 *     request details.
 *
 * Usage:
 *   ./mini_web_server epoll <ip> <port>
 *   ./EpollServer <ip> <port>
 *
 * Contrast with other modes:
 *   - select mode: fd_set limited to FD_SETSIZE (1024), O(n) scan
 *   - epoll mode:  no fd limit (system-wide), O(1) ready-fd delivery,
 *     better performance under high concurrency
 */

#define EPOLL_MAX_EVENTS   256
#define EPOLL_MAX_CONNS   1024

/*
 * Run an epoll-based TCP/HTTP server on the given host:port.
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int epoll_server_run(const char *host, int port);

#endif /* EPOLL_SERVER_H */
