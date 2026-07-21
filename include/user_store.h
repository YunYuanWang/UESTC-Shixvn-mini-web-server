#ifndef USER_STORE_H
#define USER_STORE_H

#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>

/*
 * v1.4: Shared-memory user store with offset-based pointers.
 *
 * All data lives in a single mmap(MAP_SHARED|MAP_ANONYMOUS) region.
 * Pointers are replaced with int32_t byte-offsets from the base,
 * making the data fork-safe: workers share the same physical pages.
 *
 * A process-shared mutex protects write operations (add/delete).
 */

/* ---- sizing ---- */
#define USER_STORE_MAX_NODES  200000      /* 100K existing + 100K growth */
#define USER_STORE_POOL_NODES (USER_STORE_MAX_NODES + 1000)

/* ---- shared memory header (at offset 0) ---- */
typedef struct {
    pthread_mutex_t lock;           /* process-shared mutex */
    pthread_mutexattr_t lock_attr;
    int32_t head_off;               /* offset of linked-list head ListNode */
    int32_t root_off;               /* offset of RBT root BSTnode */
    int32_t nil_off;                /* offset of RBT NIL sentinel */
    int32_t size;                   /* current user count */
    int32_t list_next_free;         /* next free ListNode slot index */
    int32_t tree_next_free;         /* next free BSTnode slot index */
    int32_t _pad[2];
} shm_header_t;

/* ---- user data element ---- */
typedef struct UsrInfo {
    char name[64];
    char password[64];
    char birthdate[16];
    char phone[32];
    char mobile[32];
    char email[64];
} ElemType;

/* ---- linked list node (offset-based) ---- */
typedef struct {
    ElemType data;
    int32_t  next_off;              /* offset to next ListNode, 0 = NULL */
    int32_t  used;                  /* 1 = in use, 0 = free */
    int32_t  _pad;
} ListNode;

/* ---- RBT node (offset-based) ---- */
typedef struct {
    int32_t user_off;               /* offset to ListNode */
    int32_t left_off;
    int32_t right_off;
    int32_t parent_off;
    int32_t color;                  /* 0=RED, 1=BLACK */
    int32_t used;
} BSTnode;

/* ---- legacy typedef for compatibility ---- */
typedef ListNode *ListPtr;

/* ---- offset conversion macros ---- */
#define PTR(base, off, type)  ((off) ? (type *)((char *)(base) + (off)) : NULL)
#define OFF(base, ptr)        ((ptr) ? (int32_t)((char *)(ptr) - (char *)(base)) : 0)

/* ---- API: called by master before fork ---- */
void *user_store_shm_init(void);         /* mmap + init header + NIL, returns base */
void  user_store_shm_load_csv(void *base, const char *path);  /* load CSV into SHM */

/* ---- API: usable by any process (workers + master) ---- */
ListNode *user_store_find(const char *name);
ListNode *user_store_find_with_steps(const char *name, int *steps, int verbose);
int       user_store_add(const char *csv_line);
int       user_store_delete(const char *name);
void      user_store_free(void);
void      user_store_print_index(void);
void      user_store_format_users(char *buf, int buf_size, int *total, int *offset);
ListNode *user_store_find_index(const char *name);
void      user_store_compare_search_method(const char *name, int verbose);

/* ---- v1.4: multi-field AND search ---- */
typedef struct {
    char name[64];
    char phone[32];
    char mobile[32];
    char email[64];
} search_criteria_t;

int user_store_search(const search_criteria_t *criteria, char *buf, int buf_size);

/* v1.6: authenticate a CSV user by mobile + password */
int user_store_auth(const char *mobile, const char *password);

/* v1.7: look up a user's display name by login ID (mobile or name) */
const char *user_store_lookup_name(const char *login_id);

#endif
