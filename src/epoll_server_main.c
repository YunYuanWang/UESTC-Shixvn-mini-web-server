/*
 * epoll_server_main.c — standalone entry point for EpollServer binary
 *
 * Usage: ./EpollServer <ip> <port>
 * Example: ./EpollServer 127.0.0.3 8888
 *
 * This is a thin wrapper that initializes logging and user data, then
 * delegates to epoll_server_run().  The same epoll server code is also
 * available via the mini_web_server dispatch:
 *   ./mini_web_server epoll <ip> <port>
 */

#include "../include/epoll_server.h"
#include "../include/log.h"
#include "../include/user_store.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    const char *host;
    int         port;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.3 8888\n", argv[0]);
        return 1;
    }

    host = argv[1];
    port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "ERROR: invalid port '%s'\n", argv[2]);
        return 1;
    }

    if (log_init_single("logs/server.log") != 0) {
        fprintf(stderr, "failed to open log file\n");
        return 1;
    }

    if (user_store_load_csv("data/users.csv") < 0) {
        fprintf(stderr, "error: cannot open data/users.csv\n");
        log_close();
        return 1;
    }

    int ret = epoll_server_run(host, port);

    log_close();
    user_store_free();
    return ret;
}
