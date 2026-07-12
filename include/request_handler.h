#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

typedef struct {
    char method[16];
    char path[256];
    char body[512];   /* CSV data for POST /users (addusr) */
} request_t;

/*
 * Parse a single-line request (e.g. "GET /hello") into a request_t.
 * Returns 0 on success, -1 on parse error.
 */
int request_handler_parse(const char *line, request_t *req);

/*
 * Set the body for operations that carry data (POST /users).
 */
void request_handler_set_body(const char *body, request_t *req);

/*
 * Process a parsed request and generate full output (connection info
 * header + response body) into the provided buffer.
 * Returns 0 on success, -1 if the buffer is too small.
 * Requires the user store to be loaded before calling.
 *
 * Supported routes:
 *   GET  /hello                        — hello response
 *   GET  /user/<name>                  — find user
 *   GET  /users                        — list all users (BST inorder)
 *   GET  /users/<name>                 — find user by name
 *   GET  /users/find-index/<name>      — find via BST index
 *   GET  /users/compare/<name>         — compare search methods
 *   GET  /users/compare-verbose/<name> — compare (verbose)
 *   POST /users                        — add user (body = csv line)
 *   DELETE /users/<name>              — delete user
 */
int request_handler_process(const request_t *req, char *output, int size);

/*
 * Process a parsed request and generate an HTTP/1.1 response into the
 * provided buffer.  Returns 0 on success, -1 if the buffer is too small.
 *
 * Supported routes are the same as request_handler_process.
 * Response format:
 *   HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: N\r\n\r\n<body>
 *   HTTP/1.1 404 NOT FOUND\r\n...
 *   HTTP/1.1 400 BAD REQUEST\r\n...
 */
int request_handler_process_http(const request_t *req, char *output, int size);

#endif
