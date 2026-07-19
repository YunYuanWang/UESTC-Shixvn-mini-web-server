/*
 * epoll_server.c — I/O multiplexing TCP/HTTP server using epoll()
 *
 * Architecture:
 *   - Single-threaded event loop using epoll_create1 / epoll_ctl /
 *     epoll_wait to monitor all active file descriptors.
 *   - Level-triggered (LT) by default — same semantics as select(),
 *     but O(1) event delivery instead of O(n) fd scan.
 *   - When listen fd is ready (EPOLLIN), accept() a new connection,
 *     add client fd to the epoll interest list.
 *   - When a client fd is ready (EPOLLIN), recv() the HTTP request,
 *     process it via request_handler, send HTTP/1.1 response, handle
 *     keep-alive or close.
 *   - Real-time stdout: connection count, client address, request details.
 *
 * Connection tracking:
 *   A fixed-size array of conn_t records.  Each epoll_event's data.ptr
 *   points directly to the conn_t for O(1) lookup (no slot scanning).
 *
 * Signal handling:
 *   - SIGINT:  sets g_shutdown flag; epoll_wait() returns EINTR.
 *   - SIGPIPE: ignored (SIG_IGN).
 */

#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/http_parser.h"
#include "../include/epoll_server.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- per-connection state ---- */
#define RECV_BUF_SIZE  8192
#define RESP_BUF_SIZE (5 * 1024 * 1024 + 4096)  /* v1.2: 5MB + headers for images */

typedef struct {
    int    fd;               /* -1 = slot free */
    char   client_ip[INET_ADDRSTRLEN];
    int    client_port;
    char   recv_buf[RECV_BUF_SIZE];
    int    recv_len;
    int    request_count;    /* requests handled on this connection */
    time_t last_activity;    /* timestamp of last data activity */
} conn_t;

/* ---- v1.1: graceful close to avoid RST on keep-alive connections ---- */
static void safe_close(int fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        /* drain any pending data to avoid RST */
        char dummy[256];
        while (recv(fd, dummy, sizeof(dummy), 0) > 0) {}
        close(fd);
    }
}

/* ---- global state ---- */
static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_worker_shutdown = 0;  /* v1.0: set by SIGTERM in worker mode */
static int                   g_active_conns = 0;   /* current active connections */
static int                   g_worker_mode = 0;    /* v1.0: non-zero when running as worker */
static int                   g_worker_id   = 0;    /* v1.1: worker number (1-based) */
static int                   g_master_listen_fd = -1; /* v1.0: listen_fd inherited from master */

static void sigint_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* v1.0: SIGTERM handler for worker graceful shutdown */
static void sigterm_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
    g_worker_shutdown = 1;
}

/*
 * Print connection count to stdout in real time.
 */
static void print_conn_count(void) {
    printf("[EpollServer] active connections: %d\n", g_active_conns);
    fflush(stdout);
}

/* ================================================================
 *  epoll_server_run
 * ================================================================ */
int epoll_server_run(const char *host, int port) {
    int          listen_fd;
    int          epfd;
    struct sockaddr_in server_addr;
    int          optval;
    conn_t      *connections = NULL;
    struct epoll_event ev;
    struct epoll_event events[EPOLL_MAX_EVENTS];
    int          clients_served = 0;
    int          ret = -1;
    char         msg[1024];

    /* ---- allocate connection array ---- */
    connections = (conn_t *)calloc(EPOLL_MAX_CONNS, sizeof(conn_t));
    if (connections == NULL) {
        log_error("[EpollServer] failed to allocate connection array");
        return -1;
    }
    for (int i = 0; i < EPOLL_MAX_CONNS; i++) {
        connections[i].fd = -1;
    }

    /* ---- install signal handlers ---- */
    {
        struct sigaction sa;
        sa.sa_handler = sigint_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);

        /* v1.0: also handle SIGTERM (sent by master to workers) */
        sa.sa_handler = sigterm_handler;
        sigaction(SIGTERM, &sa, NULL);

        /* SIGPIPE: ignore so send() returns EPIPE instead of killing us */
        signal(SIGPIPE, SIG_IGN);
    }

    /* ---- startup banner ---- */
    log_info("========================================");
    {
        char buf[64];
        if (g_worker_mode) {
            snprintf(buf, sizeof(buf),
                     "  Worker-%d PID: %d  (epoll I/O multiplexing)",
                     g_worker_id, (int)getpid());
        } else {
            snprintf(buf, sizeof(buf),
                     "  EpollServer PID: %d  (epoll I/O multiplexing mode)",
                     (int)getpid());
        }
        log_info(buf);
    }
    if (!g_worker_mode) {
        snprintf(msg, sizeof(msg),
                 "  listening on %s:%d  max_events: %d  max_connections: %d",
                 host, port, EPOLL_MAX_EVENTS, EPOLL_MAX_CONNS);
        log_info(msg);
    }
    log_info("========================================");

    /* ---- create socket (or reuse master's in worker mode) ---- */
    if (g_worker_mode) {
        listen_fd = g_master_listen_fd;
        snprintf(msg, sizeof(msg),
                 "[Worker] using inherited listen_fd=%d", listen_fd);
        log_info(msg);
    } else {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            log_error("[EpollServer] socket() failed");
            goto cleanup;
        }
        log_info("[EpollServer] socket created");

        /* ---- SO_REUSEADDR ---- */
        optval = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                       &optval, sizeof(optval)) < 0) {
            log_error("[EpollServer] setsockopt(SO_REUSEADDR) failed");
            goto cleanup;
        }

        /* ---- bind ---- */
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port   = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
            snprintf(msg, sizeof(msg),
                     "[EpollServer] invalid address: %s", host);
            log_error(msg);
            fprintf(stderr, "ERROR: invalid address '%s'\n", host);
            goto cleanup;
        }

        if (bind(listen_fd, (struct sockaddr *)&server_addr,
                 sizeof(server_addr)) < 0) {
            snprintf(msg, sizeof(msg),
                     "[EpollServer] bind(%s:%d) failed", host, port);
            log_error(msg);
            fprintf(stderr, "ERROR: bind(%s:%d) failed — "
                    "port may already be in use\n", host, port);
            goto cleanup;
        }

        snprintf(msg, sizeof(msg),
                 "[EpollServer] listening on %s:%d", host, port);
        log_info(msg);
        printf("EpollServer listening on http://%s:%d  (Ctrl-C to stop)\n",
               host, port);
        fflush(stdout);

        /* ---- listen ---- */
        if (listen(listen_fd, SOMAXCONN) < 0) {
            log_error("[EpollServer] listen() failed");
            goto cleanup;
        }
    }

    /* ---- create epoll instance ---- */
    epfd = epoll_create1(0);
    if (epfd < 0) {
        log_error("[EpollServer] epoll_create1() failed");
        goto cleanup;
    }

    /* ---- add listen_fd to epoll ---- */
    ev.events  = EPOLLIN;
    ev.data.ptr = NULL;  /* listen_fd is identified by ptr == NULL + fd check */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        log_error("[EpollServer] epoll_ctl(ADD, listen_fd) failed");
        goto cleanup;
    }

    printf("[EpollServer] active connections: %d\n", g_active_conns);
    fflush(stdout);

    /* ================================================================
     *  main event loop
     * ================================================================ */
    while (!g_shutdown) {
        int nfds;
        int timeout_ms = 1000;  /* 1-second timeout for idle checks */

        nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) {
                /* interrupted by SIGINT — exit loop */
                if (g_shutdown) break;
                continue;
            }
            log_error("[EpollServer] epoll_wait() failed");
            break;
        }

        if (nfds == 0) {
            /* timeout — check for idle keep-alive connections */
            time_t now = time(NULL);
            for (int i = 0; i < EPOLL_MAX_CONNS; i++) {
                if (connections[i].fd != -1 &&
                    now - connections[i].last_activity >= KEEP_ALIVE_TIMEOUT_SEC) {
                    snprintf(msg, sizeof(msg),
                             "[EpollServer] idle timeout on fd=%d (slot %d), "
                             "closing after %d request(s)",
                             connections[i].fd, i,
                             connections[i].request_count);
                    log_info(msg);
                    printf("[-] idle timeout: %s:%d (fd=%d, %d request(s))\n",
                           connections[i].client_ip,
                           connections[i].client_port,
                           connections[i].fd,
                           connections[i].request_count);
                    fflush(stdout);

                    epoll_ctl(epfd, EPOLL_CTL_DEL, connections[i].fd, NULL);
                    safe_close(connections[i].fd);
                    connections[i].fd = -1;
                    g_active_conns--;
                    print_conn_count();
                }
            }
            continue;
        }

        /* ---- process all ready events ---- */
        for (int i = 0; i < nfds; i++) {
            int ready_fd;

            /*
             * For listen_fd we stored NULL in data.ptr, so we need to
             * check against listen_fd.  For client fds, data.ptr points
             * directly to the conn_t.
             */
            if (events[i].data.ptr == NULL) {
                ready_fd = listen_fd;
            } else {
                conn_t *c = (conn_t *)events[i].data.ptr;
                ready_fd = c->fd;
            }

            /*
             * Check for error/hangup conditions first.
             */
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (ready_fd != listen_fd) {
                    conn_t *conn = (conn_t *)events[i].data.ptr;
                    snprintf(msg, sizeof(msg),
                             "[EpollServer] error/hangup on fd=%d", ready_fd);
                    log_info(msg);
                    printf("[-] client disconnected (error): %s:%d\n",
                           conn->client_ip, conn->client_port);
                    fflush(stdout);

                    epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, NULL);
                    safe_close(ready_fd);
                    conn->fd = -1;
                    g_active_conns--;
                    print_conn_count();
                }
                continue;
            }

            if (ready_fd == listen_fd) {
                /* ============================================
                 *  new incoming connection
                 * ============================================ */
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int conn_fd;

                conn_fd = accept(listen_fd,
                                 (struct sockaddr *)&client_addr,
                                 &client_len);
                if (conn_fd < 0) {
                    if (errno == EINTR) continue;
                    log_error("[EpollServer] accept() failed");
                    continue;
                }

                /* find a free slot */
                {
                    int slot = -1;
                    for (int j = 0; j < EPOLL_MAX_CONNS; j++) {
                        if (connections[j].fd == -1) {
                            slot = j;
                            break;
                        }
                    }
                    if (slot == -1) {
                        log_error("[EpollServer] connection table full — "
                                  "rejecting new client");
                        fprintf(stderr,
                                "ERROR: connection table full (max %d)\n",
                                EPOLL_MAX_CONNS);
                        close(conn_fd);
                        continue;
                    }

                    connections[slot].fd             = conn_fd;
                    connections[slot].recv_len       = 0;
                    connections[slot].request_count  = 0;
                    connections[slot].last_activity  = time(NULL);
                    connections[slot].client_port    = ntohs(client_addr.sin_port);
                    memset(connections[slot].recv_buf, 0, RECV_BUF_SIZE);
                    inet_ntop(AF_INET, &client_addr.sin_addr,
                              connections[slot].client_ip,
                              sizeof(connections[slot].client_ip));

                    /* add to epoll — store conn_t pointer for O(1) lookup */
                    ev.events   = EPOLLIN;
                    ev.data.ptr = &connections[slot];
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
                        log_error("[EpollServer] epoll_ctl(ADD, client) failed");
                        close(conn_fd);
                        connections[slot].fd = -1;
                        continue;
                    }

                    /* determine client type based on port range */
                    {
                        const char *client_type;
                        if (client_addr.sin_port == 0) {
                            client_type = "unknown";
                        } else if (ntohs(client_addr.sin_port) < 1024) {
                            client_type = "privileged";
                        } else if (ntohs(client_addr.sin_port) < 10000) {
                            client_type = "standard";
                        } else {
                            client_type = "ephemeral";
                        }

                        snprintf(msg, sizeof(msg),
                                 "%s[#%d] accepted connection "
                                 "from %s:%d (fd=%d) type=%s",
                                 g_worker_mode ? "" : "[EpollServer] ",
                                 clients_served + 1,
                                 connections[slot].client_ip,
                                 connections[slot].client_port,
                                 conn_fd,
                                 client_type);
                        log_info(msg);

                        printf("[+] [#%d] client connected: %s:%d "
                               "(fd=%d, type=%s)\n",
                               clients_served + 1,
                               connections[slot].client_ip,
                               connections[slot].client_port,
                               conn_fd,
                               client_type);
                        fflush(stdout);
                    }
                }

                clients_served++;
                g_active_conns++;
                print_conn_count();

            } else {
                /* ============================================
                 *  data from an existing client
                 * ============================================ */
                conn_t *conn = (conn_t *)events[i].data.ptr;
                ssize_t n;

                n = recv(ready_fd, conn->recv_buf + conn->recv_len,
                         RECV_BUF_SIZE - conn->recv_len - 1, 0);

                if (n <= 0) {
                    /* client closed or error */
                    if (n < 0) {
                        snprintf(msg, sizeof(msg),
                                 "[EpollServer] recv() error on fd=%d", ready_fd);
                        log_error(msg);
                    }
                    printf("[-] client disconnected: %s:%d (fd=%d)\n",
                           conn->client_ip, conn->client_port, ready_fd);
                    fflush(stdout);

                    epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, NULL);
                    safe_close(ready_fd);
                    conn->fd = -1;
                    g_active_conns--;
                    print_conn_count();
                    continue;
                }

                conn->recv_len += (int)n;
                conn->recv_buf[conn->recv_len] = '\0';
                conn->last_activity = time(NULL);

                /*
                 * Log the raw request from the client.
                 */
                {
                    char first_line[256];
                    char *nl;
                    strncpy(first_line, conn->recv_buf, sizeof(first_line) - 1);
                    first_line[sizeof(first_line) - 1] = '\0';
                    nl = strchr(first_line, '\r');
                    if (nl != NULL) *nl = '\0';
                    nl = strchr(first_line, '\n');
                    if (nl != NULL) *nl = '\0';

                    printf("[client %s:%d] %s\n",
                           conn->client_ip, conn->client_port, first_line);
                    fflush(stdout);
                }

                /*
                 * Process the HTTP request.
                 */
                {
                    static char resp_buf[RESP_BUF_SIZE];  /* v1.2: .bss to avoid stack overflow */
                    char *method = NULL;
                    char *path   = NULL;
                    char line_copy[512];
                    request_t req;
                    char *nl;
                    int resp_status;
                    ssize_t sent;

                    /* ---- parse HTTP request line ---- */
                    strncpy(line_copy, conn->recv_buf,
                            sizeof(line_copy) - 1);
                    line_copy[sizeof(line_copy) - 1] = '\0';
                    nl = strchr(line_copy, '\r');
                    if (nl != NULL) *nl = '\0';
                    nl = strchr(line_copy, '\n');
                    if (nl != NULL) *nl = '\0';

                    method = line_copy;
                    nl = strchr(line_copy, ' ');

                    /*
                     * Check if this looks like an HTTP request.
                     * Valid HTTP methods: GET, POST, PUT, DELETE, HEAD,
                     * OPTIONS, PATCH, TRACE, CONNECT.
                     * If not, treat as raw message and echo back.
                     */
                    {
                        int is_http = 0;
                        const char *http_methods[] = {
                            "GET", "POST", "PUT", "DELETE", "HEAD",
                            "OPTIONS", "PATCH", "TRACE", "CONNECT", NULL
                        };
                        char first_word[16] = {0};

                        if (nl != NULL) {
                            size_t wlen = (size_t)(nl - line_copy);
                            if (wlen >= sizeof(first_word)) {
                                wlen = sizeof(first_word) - 1;
                            }
                            memcpy(first_word, line_copy, wlen);
                        } else {
                            strncpy(first_word, line_copy,
                                    sizeof(first_word) - 1);
                        }

                        for (int m = 0; http_methods[m] != NULL; m++) {
                            if (strcmp(first_word,
                                       http_methods[m]) == 0) {
                                is_http = 1;
                                break;
                            }
                        }

                        if (!is_http) {
                            /*
                             * Raw message — echo it back to the
                             * client with a simple acknowledgment.
                             */
                            char first_line[512];
                            char *cr;
                            strncpy(first_line, conn->recv_buf,
                                    sizeof(first_line) - 1);
                            first_line[sizeof(first_line) - 1] = '\0';
                            cr = strchr(first_line, '\r');
                            if (cr != NULL) *cr = '\0';
                            cr = strchr(first_line, '\n');
                            if (cr != NULL) *cr = '\0';

                            snprintf(resp_buf, sizeof(resp_buf),
                                     "[EpollServer] received: %s\r\n",
                                     first_line);
                            send(ready_fd, resp_buf,
                                 strlen(resp_buf), 0);

                            snprintf(msg, sizeof(msg),
                                     "[EpollServer] raw message from "
                                     "%s:%d: %s",
                                     conn->client_ip,
                                     conn->client_port,
                                     first_line);
                            log_info(msg);

                            /* keep connection alive for more messages */
                            conn->recv_len = 0;
                            memset(conn->recv_buf, 0, RECV_BUF_SIZE);
                            conn->request_count++;
                            continue;
                        }

                        if (nl == NULL) {
                            /* HTTP method but no path — malformed */
                            snprintf(resp_buf, sizeof(resp_buf),
                                     "HTTP/1.1 400 BAD REQUEST\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 17\r\n"
                                     "Connection: close\r\n"
                                     "\r\n"
                                     "400 Bad Request\n");
                            send(ready_fd, resp_buf,
                                 strlen(resp_buf), 0);
                            epoll_ctl(epfd, EPOLL_CTL_DEL,
                                      ready_fd, NULL);
                            safe_close(ready_fd);
                            conn->fd = -1;
                            g_active_conns--;
                            log_info("[EpollServer] response: "
                                     "(malformed) -> 400 BAD REQUEST");
                            printf("[-] malformed request, "
                                   "disconnecting: %s:%d\n",
                                   conn->client_ip,
                                   conn->client_port);
                            print_conn_count();
                            continue;
                        }
                    }
                    *nl = '\0';
                    path = nl + 1;
                    while (*path == ' ') path++;

                    nl = strchr(path, ' ');
                    if (nl != NULL) *nl = '\0';

                    /* ---- fill request_t ---- */
                    memset(&req, 0, sizeof(req));
                    {
                        int k;
                        for (k = 0;
                             method[k] != '\0' &&
                             k < (int)sizeof(req.method) - 1;
                             k++) {
                            req.method[k] = (char)toupper(
                                (unsigned char)method[k]);
                        }
                        req.method[k] = '\0';
                    }
                    strncpy(req.path, path, sizeof(req.path) - 1);
                    req.path[sizeof(req.path) - 1] = '\0';

                    /* extract body (if present) */
                    {
                        char *body_start;
                        body_start = strstr(conn->recv_buf, "\r\n\r\n");
                        if (body_start != NULL) {
                            body_start += 4;
                            if (body_start < conn->recv_buf +
                                               conn->recv_len &&
                                body_start[0] != '\0') {
                                strncpy(req.body, body_start,
                                        sizeof(req.body) - 1);
                                req.body[sizeof(req.body) - 1] = '\0';
                                /* strip trailing \r\n */
                                {
                                    char *end = req.body +
                                                strlen(req.body);
                                    while (end > req.body &&
                                           (*(end - 1) == '\r' ||
                                            *(end - 1) == '\n')) {
                                        end--;
                                        *end = '\0';
                                    }
                                }
                            }
                        }
                    }

                    /* ---- v1.1: keep-alive negotiation (case-insensitive header matching) ---- */
                    {
                        int close_after = 0;
                        /*
                         * AB sends HTTP/1.0 + "Connection: Keep-Alive" (mixed case).
                         * Curl sends "Connection: keep-alive" (lowercase).
                         * We detect the keep-alive keyword case-insensitively
                         * by checking all 4 case variants.
                         */
                        #define HAS_KEEPALIVE(haystack) \
                            (strstr(haystack, "Connection: keep-alive") || \
                             strstr(haystack, "Connection: Keep-Alive") || \
                             strstr(haystack, "connection: keep-alive") || \
                             strstr(haystack, "connection: Keep-Alive"))
                        #define HAS_CLOSE(haystack) \
                            (strstr(haystack, "Connection: close") || \
                             strstr(haystack, "Connection: Close") || \
                             strstr(haystack, "connection: close") || \
                             strstr(haystack, "connection: Close"))

                        if (strstr(conn->recv_buf, "HTTP/1.0")) {
                            close_after = 1;  /* HTTP/1.0 defaults to close */
                            if (HAS_KEEPALIVE(conn->recv_buf)) {
                                close_after = 0;  /* explicit keep-alive */
                            }
                        } else {
                            /* HTTP/1.1 defaults to keep-alive */
                            if (HAS_CLOSE(conn->recv_buf)) {
                                close_after = 1;  /* explicit close */
                            }
                        }
                        req.keep_alive = close_after ? 0 : 1;
                        #undef HAS_KEEPALIVE
                        #undef HAS_CLOSE
                    }

                    /* ---- generate HTTP/1.1 response ---- */
                    {
                        int resp_len = request_handler_process_http(&req, resp_buf,
                                                     sizeof(resp_buf));
                        if (resp_len < 0) {
                            resp_len = snprintf(resp_buf, sizeof(resp_buf),
                                     "HTTP/1.1 500 INTERNAL SERVER ERROR\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 25\r\n"
                                     "Connection: close\r\n"
                                     "\r\n"
                                     "500 Internal Server Error\n");
                        }

                        /* extract and log response status (v1.1: Worker-N prefix in worker mode) */
                        if (strncmp(resp_buf, "HTTP/1.1 ", 9) == 0) {
                            resp_status = atoi(resp_buf + 9);
                            if (g_worker_mode) {
                                snprintf(msg, sizeof(msg),
                                         "[Worker-%d] fd=%d %s %s -> %d",
                                         g_worker_id, ready_fd, method, path, resp_status);
                            } else {
                                snprintf(msg, sizeof(msg),
                                         "[EpollServer] fd=%d %s %s -> %d",
                                         ready_fd, method, path, resp_status);
                            }
                        } else {
                            resp_status = 0;
                            snprintf(msg, sizeof(msg),
                                     "[EpollServer] fd=%d %s %s -> (unknown status)",
                                     ready_fd, method, path);
                        }
                        log_info(msg);

                        printf("[client %s:%d] %s %s -> %d\n",
                               conn->client_ip, conn->client_port,
                               method, path, resp_status);
                        fflush(stdout);

                        /* ---- v1.1: patch Connection header if HTTP wants close ---- */
                        if (!req.keep_alive) {
                            char *ka = strstr(resp_buf, "Connection: Keep-Alive");
                            if (ka) {
                                /* "Keep-Alive" is 10 chars → pad "close" to same length */
                                memcpy(ka + 12, "close     ", 10);
                            }
                        }

                        /* ---- send response (v1.2: use actual length for binary) ---- */
                        sent = send(ready_fd, resp_buf, (size_t)resp_len, 0);
                    }
                    if (sent < 0) {
                        snprintf(msg, sizeof(msg),
                                 "[EpollServer] send() failed on fd=%d",
                                 ready_fd);
                        log_error(msg);
                        /* send failure — close connection */
                        epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, NULL);
                        safe_close(ready_fd);
                        conn->fd = -1;
                        g_active_conns--;
                        log_info("[EpollServer] connection closed "
                                 "(send error)");
                        printf("[-] send error, disconnecting: %s:%d\n",
                               conn->client_ip, conn->client_port);
                        print_conn_count();
                        continue;
                    }

                    /* ---- v1.2: write access log ---- */
                    log_access(conn->client_ip, method, path,
                               resp_status, (int)sent);

                    /* ---- keep-alive: check limits + HTTP version negotiation ---- */
                    conn->request_count++;

                    if (conn->request_count >= MAX_KEEP_ALIVE_REQUESTS || !req.keep_alive) {
                        /* max requests reached or HTTP wants close — close connection */
                        if (!req.keep_alive) {
                            snprintf(msg, sizeof(msg),
                                     "[EpollServer] HTTP close requested "
                                     "on fd=%d, closing", ready_fd);
                        } else {
                            snprintf(msg, sizeof(msg),
                                     "[EpollServer] max requests (%d) reached "
                                     "on fd=%d, closing",
                                     MAX_KEEP_ALIVE_REQUESTS, ready_fd);
                        }
                        log_info(msg);

                        printf("[-] max requests (%d), disconnecting: "
                               "%s:%d (fd=%d)\n",
                               MAX_KEEP_ALIVE_REQUESTS,
                               conn->client_ip, conn->client_port,
                               ready_fd);
                        fflush(stdout);

                        epoll_ctl(epfd, EPOLL_CTL_DEL, ready_fd, NULL);
                        safe_close(ready_fd);
                        conn->fd = -1;
                        g_active_conns--;
                        log_info("[EpollServer] connection closed "
                                 "(max requests)");
                        print_conn_count();
                    } else {
                        /* keep connection alive for next request */
                        conn->recv_len = 0;
                        memset(conn->recv_buf, 0, RECV_BUF_SIZE);
                        log_info("[EpollServer] keep-alive: "
                                 "waiting for next request");
                    }
                }
            }
        }
    }

    /* ---- shutdown: close all remaining connections ---- */
    log_info("[EpollServer] shutting down...");

    for (int i = 0; i < EPOLL_MAX_CONNS; i++) {
        if (connections[i].fd != -1) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, connections[i].fd, NULL);
            safe_close(connections[i].fd);
            connections[i].fd = -1;
            g_active_conns--;
        }
    }

    /* close listen socket and epoll fd (listen_fd owned by master in worker mode) */
    if (!g_worker_mode) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, listen_fd, NULL);
        close(listen_fd);
    }
    close(epfd);

    if (g_worker_mode) {
        snprintf(msg, sizeof(msg),
                 "[Worker] shutdown — served %d client(s)%s",
                 clients_served,
                 g_worker_shutdown ? " (SIGTERM)" : "");
    } else {
        snprintf(msg, sizeof(msg),
                 "[EpollServer] server shutdown — served %d client(s)%s",
                 clients_served,
                 g_shutdown ? " (SIGINT)" : "");
    }
    log_info(msg);
    if (!g_worker_mode) {
        printf("EpollServer: served %d clients, exiting%s.\n",
               clients_served,
               g_shutdown ? " (Ctrl-C)" : "");
    }

    ret = 0;

cleanup:
    free(connections);
    /* close listen_fd if we bailed out early (goto cleanup);
     * in worker mode the listen_fd is owned by master */
    if (ret == -1 && listen_fd >= 0 && !g_worker_mode) {
        close(listen_fd);
    }
    if (epfd >= 0) {
        close(epfd);
    }
    return ret;
}

/*
 * epoll_server_worker_run — v1.0 worker entry point.
 *
 * Called by each forked worker process.  Uses the listen_fd inherited from
 * the master (already bound and listening).  Installs signal handlers for
 * graceful shutdown (SIGINT / SIGTERM → g_shutdown).
 *
 * Returns 0 on clean shutdown.
 */
int epoll_server_worker_run(int listen_fd, int worker_id) {
    g_worker_mode       = 1;
    g_worker_id         = worker_id;
    g_master_listen_fd  = listen_fd;
    g_shutdown          = 0;
    g_worker_shutdown   = 0;

    /* host/port are ignored in worker mode (guarded by g_worker_mode) */
    return epoll_server_run("0.0.0.0", 0);
}
