/*
 * route_table.c — Config-based routing table implementation (v1.5)
 *
 * Each entry is a (method, path, match_type, handler) quadruple.
 *
 * Lookup strategy:
 *   - Exact matches:  djb2 hash table → O(1) average case
 *   - Prefix matches: linear scan over prefix-only array → O(m), m ≈ 10
 *
 * 405 (method-not-allowed) detection:
 *   - Hash miss + path exists in exact array with different method → 405
 *   - Hash miss + path_prefix match with different method → 405
 */

#include "../include/route_table.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 *  Handler name registry
 * ================================================================ */

typedef struct {
    const char    *name;
    handler_type_t type;
} handler_registry_t;

static const handler_registry_t g_handler_registry[] = {
    { "hello",                 HANDLER_HELLO                 },
    { "help",                  HANDLER_HELP                  },
    { "sleep",                 HANDLER_SLEEP                 },
    { "user_list",             HANDLER_USER_LIST             },
    { "user_resource",         HANDLER_USER_BY_NAME          },
    { "user_by_name",          HANDLER_USER_BY_NAME          },
    { "user_find_index",       HANDLER_USER_FIND_INDEX       },
    { "user_compare",          HANDLER_USER_COMPARE          },
    { "user_compare_verbose",  HANDLER_USER_COMPARE_VERBOSE  },
    { "user_simple_find",      HANDLER_USER_SIMPLE_FIND      },
    { "user_add",              HANDLER_USER_ADD              },
    { "user_delete",           HANDLER_USER_DELETE           },
    { "delete_form",           HANDLER_DELETE_FORM           },
    { "search",                HANDLER_SEARCH                },
    { "index",                 HANDLER_INDEX                 },
    { "blog",                  HANDLER_BLOG                  },
    { "static",                HANDLER_STATIC                },
    { "login",                 HANDLER_LOGIN                 },
    { "logout",                HANDLER_LOGOUT                },
    { NULL,                    HANDLER_NONE                  }
};

handler_type_t route_table_lookup_handler(const char *name) {
    int i;
    if (name == NULL) return HANDLER_NONE;
    for (i = 0; g_handler_registry[i].name != NULL; i++) {
        if (strcmp(name, g_handler_registry[i].name) == 0)
            return g_handler_registry[i].type;
    }
    return HANDLER_NONE;
}

const char *route_table_handler_name(handler_type_t h) {
    int i;
    for (i = 0; g_handler_registry[i].name != NULL; i++) {
        if (g_handler_registry[i].type == h) return g_handler_registry[i].name;
    }
    return "unknown";
}

/* ================================================================
 *  djb2 hash table for O(1) exact-match lookup
 * ================================================================ */

#define HASH_SIZE 256   /* power of 2 */

/*
 * Hash key = "METHOD:/path", e.g. "GET:/users/alice"
 * Only used for exact-match entries.
 */
static unsigned int hash_key(const char *method, const char *path) {
    unsigned int h = 5381;
    int c;
    while ((c = (unsigned char)*method++)) h = ((h << 5) + h) + c;
    h = ((h << 5) + h) + ':';  /* separator */
    while ((c = (unsigned char)*path++))   h = ((h << 5) + h) + c;
    return h & (HASH_SIZE - 1);
}

/*
 * Hash slot: stores index into exact[] array, or -1 for empty.
 * Linear probing on collision.
 */
static int g_hash[HASH_SIZE];

static void hash_init(void) {
    int i;
    for (i = 0; i < HASH_SIZE; i++) g_hash[i] = -1;
}

static void hash_insert(const route_table_t *rt, int exact_idx) {
    const route_entry_t *e = &rt->exact[exact_idx];
    unsigned int h = hash_key(e->method, e->path);
    while (g_hash[h] != -1) h = (h + 1) & (HASH_SIZE - 1);
    g_hash[h] = exact_idx;
}

/*
 * Look up an exact match in the hash table.
 * Returns index into exact[] on success, -1 if not found.
 * If hash hit but method differs, sets *different_method = 1.
 */
static int hash_lookup(const route_table_t *rt,
                        const char *method, const char *path,
                        int *different_method) {
    unsigned int h = hash_key(method, path);
    *different_method = 0;

    while (g_hash[h] != -1) {
        int idx = g_hash[h];
        const route_entry_t *e = &rt->exact[idx];
        if (strcmp(e->path, path) == 0) {
            /* Found the right path. Check method. */
            if (strcmp(e->method, method) == 0) return idx;
            *different_method = 1;
            return -1;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return -1;
}

/* ================================================================
 *  Lifecycle
 * ================================================================ */

void route_table_init(route_table_t *rt) {
    if (rt == NULL) return;
    memset(rt, 0, sizeof(*rt));
    hash_init();  /* ensure hash table starts with -1 sentinels */
}

int route_table_add(route_table_t *rt, const char *method,
                     const char *path, match_type_t match_type,
                     handler_type_t handler,
                     const char *auth_realm, const char *required_role) {
    route_entry_t *e;
    int i, count;

    if (rt == NULL || method == NULL || path == NULL) return -1;
    if (auth_realm == NULL) auth_realm = "";
    if (required_role == NULL) required_role = "";

    if (match_type == MATCH_EXACT) {
        if (rt->exact_count >= MAX_ROUTES) return -1;
        /* Duplicate check: same (method, path) in exact[] */
        for (i = 0; i < rt->exact_count; i++) {
            if (strcmp(rt->exact[i].path, path) == 0 &&
                strcmp(rt->exact[i].method, method) == 0) {
                fprintf(stderr, "ERROR: duplicate route: %s %s (exact)\n",
                        method, path);
                return -1;
            }
        }
        e = &rt->exact[rt->exact_count];
        rt->exact_count++;
        /* Rebuild hash after adding (simple approach for infrequent adds) */
        count = rt->exact_count;
    } else {
        if (rt->prefix_count >= MAX_ROUTES) return -1;
        /* Duplicate check: same (method, path) in prefix[] */
        for (i = 0; i < rt->prefix_count; i++) {
            if (strcmp(rt->prefix[i].path, path) == 0 &&
                strcmp(rt->prefix[i].method, method) == 0) {
                fprintf(stderr, "ERROR: duplicate route: %s %s (prefix)\n",
                        method, path);
                return -1;
            }
        }
        e = &rt->prefix[rt->prefix_count];
        rt->prefix_count++;
        count = rt->prefix_count;
    }

    memset(e, 0, sizeof(*e));
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    strncpy(e->method, method, sizeof(e->method) - 1);
    e->method[sizeof(e->method) - 1] = '\0';
    e->match_type = match_type;
    e->handler    = handler;
    strncpy(e->auth_realm, auth_realm, sizeof(e->auth_realm) - 1);
    e->auth_realm[sizeof(e->auth_realm) - 1] = '\0';
    strncpy(e->required_role, required_role, sizeof(e->required_role) - 1);
    e->required_role[sizeof(e->required_role) - 1] = '\0';

    /* Rebuild entire hash table after each add (config time, not hot path) */
    if (match_type == MATCH_EXACT) {
        hash_init();
        for (i = 0; i < rt->exact_count; i++)
            hash_insert(rt, i);
    }

    return 0;
}

/* ================================================================
 *  Route matching (optimized: hash for exact, linear for prefix)
 * ================================================================ */

static void build_allow_for_path(const route_table_t *rt,
                                  const char *path, match_type_t match_type,
                                  char *buf, int bufsize) {
    int i, offset = 0;
    const char *seen[16];
    int seen_count = 0;
    const route_entry_t *entries;
    int count;

    if (match_type == MATCH_EXACT) {
        entries = rt->exact; count = rt->exact_count;
    } else {
        entries = rt->prefix; count = rt->prefix_count;
    }

    buf[0] = '\0';
    for (i = 0; i < count; i++) {
        const route_entry_t *e = &entries[i];
        if (e->match_type != match_type) continue;
        if (strcmp(e->path, path) != 0) continue;

        int j, found = 0;
        for (j = 0; j < seen_count; j++) {
            if (strcmp(seen[j], e->method) == 0) { found = 1; break; }
        }
        if (found) continue;
        if (seen_count < 16) seen[seen_count++] = e->method;

        if (offset > 0)
            offset += snprintf(buf + offset, bufsize - offset, ", %s", e->method);
        else
            offset += snprintf(buf + offset, bufsize - offset, "%s", e->method);
        if (offset >= bufsize) break;
    }
}

static int path_matches_prefix(const char *pattern, const char *request_path,
                                char *captured, int cap_size) {
    size_t pattern_len = strlen(pattern);
    if (pattern_len == 0) return 0;
    if (captured) captured[0] = '\0';

    if (strncmp(request_path, pattern, pattern_len) != 0) return 0;

    if (captured && cap_size > 0) {
        const char *tail = request_path + pattern_len;
        strncpy(captured, tail, cap_size - 1);
        captured[cap_size - 1] = '\0';
    }
    return 1;
}

void route_table_find(const route_table_t *rt, const char *method,
                       const char *path, route_find_result_t *result) {
    int i, diff_method;

    if (rt == NULL || method == NULL || path == NULL || result == NULL) return;
    memset(result, 0, sizeof(*result));

    /* ================================================================
     *  Step 1: O(1) hash lookup for exact match
     * ================================================================ */
    i = hash_lookup(rt, method, path, &diff_method);

    if (i >= 0) {
        /* Perfect exact match */
        result->entry = &rt->exact[i];
        result->captured[0] = '\0';
        return;
    }

    if (diff_method) {
        /* Path exists as exact, but method differs → 405 */
        result->is_405 = 1;
        build_allow_for_path(rt, path, MATCH_EXACT,
                             result->allow, sizeof(result->allow));
        return;
    }

    /* ================================================================
     *  Step 2: Linear scan of prefix-only array
     * ================================================================ */
    for (i = 0; i < rt->prefix_count; i++) {
        const route_entry_t *e = &rt->prefix[i];

        if (path_matches_prefix(e->path, path,
                                 result->captured, sizeof(result->captured))) {
            if (strcmp(e->method, method) == 0) {
                result->entry = e;
                result->is_405 = 0;  /* clear 405 from earlier mismatches */
                return;
            }
            /* First prefix match with wrong method → 405 */
            if (!result->is_405) {
                result->is_405 = 1;
                build_allow_for_path(rt, e->path, MATCH_PREFIX,
                                     result->allow, sizeof(result->allow));
            }
        }
    }

    /* entry stays NULL → 404 */
}

/* ================================================================
 *  Validation
 * ================================================================ */

int route_table_validate(const route_table_t *rt) {
    int errors = 0;
    int i, j;

    if (rt == NULL) return 0;

    /* Validate exact entries */
    for (i = 0; i < rt->exact_count; i++) {
        const route_entry_t *e = &rt->exact[i];

        if (e->handler == HANDLER_NONE) {
            fprintf(stderr, "ERROR: route %s %s has unknown handler\n",
                    e->method, e->path);
            errors++;
        }
        for (j = i + 1; j < rt->exact_count; j++) {
            if (strcmp(e->path, rt->exact[j].path) == 0 &&
                strcmp(e->method, rt->exact[j].method) == 0) {
                fprintf(stderr, "ERROR: duplicate route: %s %s (exact)\n",
                        e->method, e->path);
                errors++;
            }
        }
    }

    /* Validate prefix entries */
    for (i = 0; i < rt->prefix_count; i++) {
        const route_entry_t *e = &rt->prefix[i];

        if (e->handler == HANDLER_NONE) {
            fprintf(stderr, "ERROR: route %s %s has unknown handler\n",
                    e->method, e->path);
            errors++;
        }
        for (j = i + 1; j < rt->prefix_count; j++) {
            if (strcmp(e->path, rt->prefix[j].path) == 0 &&
                strcmp(e->method, rt->prefix[j].method) == 0) {
                fprintf(stderr, "ERROR: duplicate route: %s %s (prefix)\n",
                        e->method, e->path);
                errors++;
            }
        }
    }

    if (errors > 0) {
        fprintf(stderr,
                "ERROR: route table validation failed with %d error(s). "
                "Server will not start.\n", errors);
        return -1;
    }
    return 0;
}
