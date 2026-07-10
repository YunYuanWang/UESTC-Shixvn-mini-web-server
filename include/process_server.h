#ifndef PROCESS_SERVER_H
#define PROCESS_SERVER_H

/*
 * Multi-process request server entry point.
 * Scans requests/*.req, forks a child for each, waits for all
 * children, and logs results. Outputs go to outputs/<name>.out.
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int process_server_run(void);

#endif
