#ifndef TCP_FORK_SERVER_H
#define TCP_FORK_SERVER_H

/*
 * Run a multi-process TCP/HTTP server using fork-per-connection:
 *   1. Create a socket and bind to the given host:port
 *   2. Listen for incoming connections
 *   3. For each client, fork a child process to handle the request
 *   4. Parent reaps children via SIGCHLD handler (waitpid WNOHANG)
 *   5. Runs until SIGINT (Ctrl-C) is received
 *
 * Signal handling:
 *   - SIGINT:  graceful shutdown (sets flag, accept returns EINTR)
 *   - SIGCHLD: reaped with waitpid(-1, &stat, WNOHANG) to avoid zombies
 *   - SIGPIPE: ignored so send() returns EPIPE instead of killing process
 *   - accept() handles EINTR by continuing
 *
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int tcp_fork_server_run(const char *host, int port);

#endif
