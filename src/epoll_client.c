/*
 * epoll_client.c — TCP test client for EpollServer
 *
 * Usage: ./epoll_client <ip> <port>
 *
 * Connects to the EpollServer, reads messages from stdin, sends each
 * line to the server, receives and prints the response.  Type "quit"
 * to exit the client.
 *
 * This client can be launched as 3 simultaneous instances to
 * demonstrate EpollServer's concurrent connection handling:
 *
 *   Terminal 1: ./epoll_client 127.0.0.3 8888
 *   Terminal 2: ./epoll_client 127.0.0.3 8888
 *   Terminal 3: ./epoll_client 127.0.0.3 8888
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE  4096

int main(int argc, char *argv[]) {
    const char *host;
    int         port;
    int         sock_fd;
    struct sockaddr_in server_addr;
    char        send_buf[BUF_SIZE];
    char        recv_buf[BUF_SIZE];
    ssize_t     n;

    /* ---- parse arguments ---- */
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

    /* ---- create socket ---- */
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    /* ---- connect to server ---- */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "ERROR: invalid address '%s'\n", host);
        close(sock_fd);
        return 1;
    }

    printf("Connecting to %s:%d ...\n", host, port);
    if (connect(sock_fd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    printf("Connected to EpollServer at %s:%d\n", host, port);
    printf("Type messages to send (type 'quit' to exit):\n");
    fflush(stdout);

    /* ---- read-send-recv loop ---- */
    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(send_buf, sizeof(send_buf), stdin) == NULL) {
            printf("\n");
            break;
        }

        /* strip trailing newline */
        {
            size_t len = strlen(send_buf);
            while (len > 0 &&
                   (send_buf[len - 1] == '\n' ||
                    send_buf[len - 1] == '\r')) {
                send_buf[len - 1] = '\0';
                len--;
            }
        }

        /* check for quit command */
        if (strcmp(send_buf, "quit") == 0) {
            printf("Exiting...\n");
            break;
        }

        /* send message to server */
        {
            size_t msg_len = strlen(send_buf);
            char   msg_with_newline[BUF_SIZE];

            {
                int written = snprintf(msg_with_newline,
                                      sizeof(msg_with_newline),
                                      "%s\r\n", send_buf);
                if (written < 0 || written >= (int)sizeof(msg_with_newline)) {
                    fprintf(stderr, "WARN: message truncated\n");
                }
            }

            n = send(sock_fd, msg_with_newline, strlen(msg_with_newline), 0);
            if (n < 0) {
                perror("send");
                break;
            }
        }

        /* receive response from server */
        n = recv(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n < 0) {
            perror("recv");
            break;
        } else if (n == 0) {
            printf("[server closed connection]\n");
            break;
        }

        recv_buf[n] = '\0';
        printf("response: %s\n", recv_buf);
        fflush(stdout);
    }

    close(sock_fd);
    printf("Disconnected.\n");
    return 0;
}
