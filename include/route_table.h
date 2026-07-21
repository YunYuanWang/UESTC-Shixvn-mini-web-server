#ifndef ROUTE_TABLE_H
#define ROUTE_TABLE_H

/*
 * route_table.h — Config-based routing table for mini_web_server v1.5
 *
 * Supports three config formats:
 *   1. Nginx-style  — location = /path { handler X; methods GET; }
 *   2. JSON         — {"method":"GET","path":"/","handler":"index"}
 *   3. YAML         — - method: GET  path: /  handler: index
 *
 * Core design: each route entry is a (method, path, handler) triple.
 * The same path+method can only appear once.  Different methods on
 * the same path map to different handlers — no "combined handler" needed.
 *
 * Route matching:
 *   - Exact path match (MATCH_EXACT) takes priority
 *   - Prefix/wildcard match (MATCH_PREFIX) with longest-prefix-first
 *   - 405 + Allow header when path matches but method doesn't
 *   - 404 when no path matches at all
 *   - Startup validation: duplicate rules, unknown handlers → refuse to start
 */

#include "request_handler.h"

/* ---- limits ---- */
#define MAX_ROUTES             128
#define MAX_HANDLER_NAME_LEN    64
#define MAX_ALLOW_HEADER_LEN   256

/* ---- path match type (exact or prefix) — "role" is reserved for auth ---- */
typedef enum {
    MATCH_EXACT  = 0,   /* exact path match, e.g. /hello */
    MATCH_PREFIX = 1    /* prefix/wildcard match, e.g. /users/* → captures tail */
} match_type_t;

/* ---- known handler types (maps handler name → C function) ---- */
typedef enum {
    HANDLER_NONE = -1,

    /* API handlers */
    HANDLER_HELLO,
    HANDLER_HELP,
    HANDLER_SLEEP,

    /* User CRUD handlers — one per (method, path) combination */
    HANDLER_USER_LIST,
    HANDLER_USER_BY_NAME,
    HANDLER_USER_FIND_INDEX,
    HANDLER_USER_COMPARE,
    HANDLER_USER_COMPARE_VERBOSE,
    HANDLER_USER_SIMPLE_FIND,
    HANDLER_USER_ADD,
    HANDLER_USER_DELETE,
    HANDLER_DELETE_FORM,
    HANDLER_SEARCH,

    /* Static file handlers */
    HANDLER_INDEX,
    HANDLER_BLOG,
    HANDLER_STATIC,

    HANDLER_COUNT
} handler_type_t;

/* ---- single route entry: (method, path, match_type, handler, auth) ---- */
typedef struct route_entry_s {
    char          path[256];       /* path pattern, e.g. "/users/", "/hello" */
    match_type_t  match_type;      /* MATCH_EXACT or MATCH_PREFIX */
    char          method[16];      /* single HTTP method: GET, POST, DELETE... */
    handler_type_t handler;        /* which C function to call */
    char          auth_realm[64];   /* v1.6: realm for WWW-Authenticate, ""=public */
    char          required_role[32];/* v1.6: required role, ""=any authenticated user */
} route_entry_t;

/* ---- route table (exact + prefix separated for fast lookup) ---- */
typedef struct route_table_s {
    route_entry_t exact[MAX_ROUTES];     /* O(1) via hash table */
    int           exact_count;
    route_entry_t prefix[MAX_ROUTES];    /* O(m) linear scan, m ≈ 5-10 */
    int           prefix_count;
} route_table_t;

/* ---- find result (returned by route_table_find) ---- */
typedef struct {
    const route_entry_t *entry;           /* matched entry, or NULL */
    char                 captured[256];   /* tail captured by PREFIX match */
    int                  is_405;          /* 1 = path matched but method didn't */
    char                 allow[MAX_ALLOW_HEADER_LEN]; /* "GET, POST" for Allow */
} route_find_result_t;

/* ---- lifecycle ---- */

void route_table_init(route_table_t *rt);

/*
 * Add a route entry.  Returns 0 on success, -1 on error.
 * Duplicates are detected when the same (method, path, role) triple
 * is added twice.
 */
int  route_table_add(route_table_t *rt, const char *method,
                     const char *path, match_type_t match_type,
                     handler_type_t handler,
                     const char *auth_realm, const char *required_role);

/*
 * Find a matching route.  Populates `result`.
 *
 * Priority:
 *   1. Exact path match with matching method  → result->entry set
 *   2. Prefix path match with matching method → result->entry set, captured filled
 *   3. Exact path match, wrong method         → result->is_405 = 1, Allow header
 *   4. Prefix path match, wrong method        → result->is_405 = 1
 *   5. No match at all                        → result->entry = NULL (caller → 404)
 */
void route_table_find(const route_table_t *rt, const char *method,
                      const char *path, route_find_result_t *result);

/*
 * Validate the route table before server starts.
 * Checks: duplicate (method+path+role), unknown handler, illegal role.
 * Prints errors to stderr.  Returns 0 if valid, -1 on error.
 */
int  route_table_validate(const route_table_t *rt);

/*
 * Convert a handler name string to handler_type_t.
 * Returns HANDLER_NONE (-1) if unknown.
 */
handler_type_t route_table_lookup_handler(const char *name);

/* Get the name string for a handler type (for error messages). */
const char *route_table_handler_name(handler_type_t h);

/*
 * Look up the handler function pointer for a given handler type.
 * Returns NULL for HANDLER_NONE.
 */
handler_fn route_table_get_handler_fn(handler_type_t type);

#endif /* ROUTE_TABLE_H */
