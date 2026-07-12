#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "config.h"

/*
 * Run a single-request TCP server:
 *   1. Create a socket and bind to config->host:config->port
 *   2. Listen for one incoming connection
 *   3. Read the HTTP request, parse it, and send back an HTTP response
 *   4. Close the connection and shut down
 *
 * Returns 0 on success, -1 on failure.
 * Requires: log_init() already called, user_store already loaded.
 */
int tcp_server_run(const server_config_t *config);

#endif
