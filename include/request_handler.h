#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

typedef struct {
    char method[16];
    char path[256];
    char body[512];   /* CSV data for POST /users (addusr) */

    /* v1.1: HTTP version and keep-alive from header parsing */
    int  http_version_major;   /* 1 */
    int  http_version_minor;   /* 0 or 1 */
    int  keep_alive;           /* 1=keep connection, 0=close */

    /* v1.4: parsed request headers for POST error handling */
    char content_type[64];       /* Content-Type header value */
    int  content_length_hdr;     /* Content-Length header value, -1 = absent */

    /* v1.6: Authorization header for HTTP Basic Auth */
    char authorization[256];     /* "Basic <base64>" or empty */

    /* v1.7: Cookie header for Session auth */
    char cookie[512];            /* raw Cookie header value */
} request_t;

/* ---- v1.5: handler function signature ---- */
/*
 * Each handler receives:
 *   req          — parsed request (method, path, body, headers)
 *   body_buf     — pre-allocated buffer for response body (BLOG_BODY_MAX)
 *   body_size    — size of body_buf
 *   captured     — tail captured from prefix match (empty for exact match)
 *   output       — final HTTP response buffer
 *   output_size  — size of output buffer
 *
 * Returns: HTTP response length on success, -1 on error.
 */
typedef int (*handler_fn)(const request_t *req,
                           char *body_buf, int body_size,
                           const char *captured,
                           char *output, int output_size);

/* ---- HTTP/1.1 keep-alive configuration ---- */
#define KEEP_ALIVE_TIMEOUT_SEC   5     /* idle timeout between requests */
#define MAX_KEEP_ALIVE_REQUESTS  10000   /* max requests per connection (v1.1: raised for ab benchmarks) */

/*
 * v1.2.1: Set the document root for the current request/thread.
 * Call this before request_handler_process_http() to route requests
 * to the correct virtual host's document root.
 * Default is "www" if never called.
 */
void request_handler_set_root(const char *root);

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
