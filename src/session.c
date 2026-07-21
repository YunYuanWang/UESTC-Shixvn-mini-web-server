/*
 * session.c — Server-side session management (v1.7)
 *
 * Uses /dev/urandom for token generation, djb2 hash table for storage.
 */

#include "../include/session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* ---- session storage ---- */
static session_t g_sessions[MAX_SESSIONS];
static int g_hash[SESSION_HASH_SIZE];

/* ---- djb2 hash ---- */
static unsigned int hash_id(const char *s) {
    unsigned int h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h & (SESSION_HASH_SIZE - 1);
}

/* ---- hex encode ---- */
static void hex_encode(const unsigned char *bytes, int len, char *out) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < len; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

void session_init(void) {
    int i;
    memset(g_sessions, 0, sizeof(g_sessions));
    for (i = 0; i < SESSION_HASH_SIZE; i++) g_hash[i] = -1;
}

const char *session_create(const char *username, const char *role) {
    unsigned char random_bytes[32];
    char token[SESSION_ID_LEN];
    int fd, i;

    /* Generate random token */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Fallback: use time + pid as weak random source */
        unsigned int seed = (unsigned int)(time(NULL) ^ getpid());
        for (i = 0; i < 32; i++) {
            seed = seed * 1103515245 + 12345;
            random_bytes[i] = (unsigned char)(seed >> 16);
        }
    } else {
        read(fd, random_bytes, 32);
        close(fd);
    }
    hex_encode(random_bytes, 32, token);

    /* Cleanup expired sessions first */
    session_cleanup_expired();

    /* Find a free slot */
    int slot = -1;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* Table full — evict oldest expired */
        session_cleanup_expired();
        for (i = 0; i < MAX_SESSIONS; i++) {
            if (!g_sessions[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return NULL;  /* really full */

    /* Fill session */
    session_t *s = &g_sessions[slot];
    memset(s, 0, sizeof(*s));
    strncpy(s->session_id, token, sizeof(s->session_id) - 1);
    strncpy(s->username, username, sizeof(s->username) - 1);
    strncpy(s->role, role, sizeof(s->role) - 1);
    s->created_at = time(NULL);
    s->expires_at = s->created_at + SESSION_TTL;
    s->used = 1;

    /* Insert into hash table */
    unsigned int h = hash_id(token);
    while (g_hash[h] != -1) h = (h + 1) & (SESSION_HASH_SIZE - 1);
    g_hash[h] = slot;

    return g_sessions[slot].session_id;
}

session_t *session_lookup(const char *session_id) {
    if (session_id == NULL || session_id[0] == '\0') return NULL;

    unsigned int h = hash_id(session_id);
    while (g_hash[h] != -1) {
        int idx = g_hash[h];
        session_t *s = &g_sessions[idx];
        if (s->used && strcmp(s->session_id, session_id) == 0) {
            /* Check expiration */
            if (time(NULL) > s->expires_at) {
                session_destroy(session_id);
                return NULL;
            }
            return s;
        }
        h = (h + 1) & (SESSION_HASH_SIZE - 1);
    }
    return NULL;
}

void session_destroy(const char *session_id) {
    if (session_id == NULL) return;

    unsigned int h = hash_id(session_id);
    while (g_hash[h] != -1) {
        int idx = g_hash[h];
        session_t *s = &g_sessions[idx];
        if (s->used && strcmp(s->session_id, session_id) == 0) {
            s->used = 0;
            g_hash[h] = -1;
            return;
        }
        h = (h + 1) & (SESSION_HASH_SIZE - 1);
    }
}

void session_cleanup_expired(void) {
    int i;
    time_t now = time(NULL);
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].used && now > g_sessions[i].expires_at) {
            /* Remove from hash */
            unsigned int h = hash_id(g_sessions[i].session_id);
            while (g_hash[h] != -1) {
                if (g_hash[h] == i) { g_hash[h] = -1; break; }
                h = (h + 1) & (SESSION_HASH_SIZE - 1);
            }
            g_sessions[i].used = 0;
        }
    }
}
