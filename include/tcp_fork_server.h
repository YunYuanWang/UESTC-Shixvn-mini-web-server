#ifndef TCP_FORK_SERVER_H
#define TCP_FORK_SERVER_H

#define FORK_MAX_CLIENTS 5

/*
 * Run a multi-process TCP/HTTP server using fork-per-connection:
 *   1. Create a socket and bind to 127.0.0.1:8080
 *   2. Listen for incoming connections
 *   3. For each client, fork a child process to handle the request
 *   4. Parent reaps children via SIGCHLD handler (waitpid WNOHANG)
 *   5. Exits after serving MAX_CLIENTS connections
 *
 * Signal handling:
 *   - SIGCHLD: reaped with waitpid(-1, &stat, WNOHANG) to avoid zombies
 *   - SIGPIPE: ignored so send() returns EPIPE instead of killing process
 *   - accept() handles EINTR by continuing
 *
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int tcp_fork_server_run(void);

#endif
