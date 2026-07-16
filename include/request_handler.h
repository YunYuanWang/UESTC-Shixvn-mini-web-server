#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

typedef struct {
    char method[16];
    char path[256];
    char body[512];   /* CSV data for POST /users (addusr) */
} request_t;

/* ---- HTTP/1.1 keep-alive configuration ---- */
#define KEEP_ALIVE_TIMEOUT_SEC   5     /* idle timeout between requests */
#define MAX_KEEP_ALIVE_REQUESTS  100   /* max requests per connection */

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

/*
 * Handle a single HTTP connection: recv -> parse -> route -> send.
 * Reads the HTTP request from conn_fd, processes it, and sends the
 * HTTP/1.1 response back.  Logs the request and response status.
 * Returns 0 on success, -1 on client/IO error.
 */
int request_handler_handle_connection(int conn_fd);

/*
 * Set a human-readable label for the current thread (e.g. 1 for "Worker-1").
 * When set, log messages from request_handler_handle_connection() will
 * include a [Worker-N] prefix.  Uses a thread-local variable so each
 * thread can set its own label independently.
 */
void request_handler_set_worker_label(int worker_id);

/*
 * Return the current thread's worker label as an integer.
 * Returns 0 if no label has been set.
 */
int request_handler_get_worker_label_int(void);

#endif
