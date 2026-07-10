#include "../include/http_response.h"
#include <stdio.h>

int build_hello_response(char *buffer, int size) {
    const char *body = "Hello, Web!\n";
    int written;

    if (buffer == NULL || size <= 0) {
        return -1;
    }

    written = snprintf(
        buffer,
        (size_t)size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "%s",
        body
    );

    if (written < 0 || written >= size) {
        return -1;
    }

    return written;
}
