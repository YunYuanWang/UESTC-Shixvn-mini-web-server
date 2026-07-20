#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include "config.h"

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
 *   - v1.2.1: name-based virtual hosting via server_name matching.
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
 * v1.2.1: Set the server config for Host-header-based virtual host routing.
 * Must be called before epoll_server_run() or epoll_server_worker_run().
 * The config pointer is stored and accessed read-only at request time.
 */
void epoll_server_set_config(const server_config_t *cfg);

/*
 * Run an epoll-based TCP/HTTP server on the given host:port.
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 *           epoll_server_set_config() optionally called for virtual hosting.
 */
int epoll_server_run(const char *host, int port);

/*
 * v1.0: Run an epoll-based TCP/HTTP worker on a pre-created listen_fd.
 *
 * The listen_fd must already be bound and listening (created by the master
 * process and inherited via fork).  The worker does NOT close listen_fd on
 * shutdown — the master owns it.
 *
 * Returns 0 on clean shutdown, -1 on error.
 * Requires: log_reopen() already called, user_store already loaded.
 *           epoll_server_set_config() already called for virtual hosting.
 */
int epoll_server_worker_run(int listen_fd, int worker_id);

#endif /* EPOLL_SERVER_H */
