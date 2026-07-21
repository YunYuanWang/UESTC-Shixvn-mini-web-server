#ifndef SESSION_H
#define SESSION_H

/*
 * session.h — Server-side session management for cookie-based auth (v1.7)
 */

#include <time.h>

#define SESSION_ID_LEN    65   /* 32 random bytes hex-encoded + null */
#define SESSION_HASH_SIZE 256
#define MAX_SESSIONS      1024
#define SESSION_TTL        3600  /* 1 hour default */

typedef struct {
    char   session_id[SESSION_ID_LEN];
    char   username[64];
    char   role[32];
    time_t created_at;
    time_t expires_at;
    int    used;
} session_t;

/* Initialize session storage */
void session_init(void);

/*
 * Create a new session for the given user.
 * Returns the session_id string (static storage, do not free).
 */
const char *session_create(const char *username, const char *role);

/*
 * Look up a session by ID. Returns NULL if not found or expired.
 */
session_t *session_lookup(const char *session_id);

/*
 * Destroy (logout) a session by ID.
 */
void session_destroy(const char *session_id);

/*
 * Remove all expired sessions. Called periodically or on each login.
 */
void session_cleanup_expired(void);

#endif /* SESSION_H */
