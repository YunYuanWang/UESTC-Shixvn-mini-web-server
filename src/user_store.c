/*
 * user_store.c — Shared-memory user store with offset-based pointers (v1.4)
 *
 * All data lives in a MAP_SHARED|MAP_ANONYMOUS mmap region.
 * Pointers are replaced with int32_t byte-offsets from the mmap base,
 * making the entire data structure fork-safe.
 *
 * Layout: [shm_header_t] [ListNode pool] [BSTnode pool]
 */

#include "../include/user_store.h"
#include "../include/user_index.h"
#include "../include/log.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ---- globals ---- */
void *g_shm_base = NULL;       /* mmap base address */
shm_header_t *g_header = NULL; /* points to offset 0 */
ListNode *g_list_pool = NULL;  /* ListNode array */
BSTnode  *g_tree_pool = NULL;  /* BSTnode array */
BSTnode  *g_nil = NULL;        /* RBT NIL sentinel (first tree slot) */
BST       g_user_bst;          /* legacy BST wrapper */

/* ---- helpers ---- */

static void *list_at(int32_t off) { return PTR(g_shm_base, off, ListNode); }
static void *tree_at(int32_t off) { return PTR(g_shm_base, off, BSTnode); }
#define LIST(off) ((ListNode *)list_at(off))
#define TREE(off) ((BSTnode *)tree_at(off))
#define OFF_L(p)   OFF(g_shm_base, p)
#define OFF_T(p)   OFF(g_shm_base, p)

static void remove_newline(char *text) {
    text[strcspn(text, "\r\n")] = '\0';
}

static int parse_csv_line(char *line, ElemType *elem) {
    char *token;
    int field;
    memset(elem, 0, sizeof(*elem));
    token = strtok(line, ",");
    for (field = 0; token != NULL; field++) {
        switch (field) {
        case 0: strncpy(elem->name, token, sizeof(elem->name) - 1); break;
        case 1: strncpy(elem->password, token, sizeof(elem->password) - 1); break;
        case 2: strncpy(elem->birthdate, token, sizeof(elem->birthdate) - 1); break;
        case 3: strncpy(elem->phone, token, sizeof(elem->phone) - 1); break;
        case 4: strncpy(elem->mobile, token, sizeof(elem->mobile) - 1); break;
        case 5: strncpy(elem->email, token, sizeof(elem->email) - 1); break;
        }
        token = strtok(NULL, ",");
    }
    return (field < 3) ? -1 : 0;
}

/* ---- allocators from shared pools ---- */

static ListNode *list_alloc(void) {
    int32_t idx = g_header->list_next_free;
    if (idx >= USER_STORE_POOL_NODES) return NULL;
    g_header->list_next_free = idx + 1;
    ListNode *n = &g_list_pool[idx];
    memset(n, 0, sizeof(ListNode));
    n->used = 1;
    return n;
}

static BSTnode *tree_alloc(void) {
    int32_t idx = g_header->tree_next_free;
    if (idx >= USER_STORE_POOL_NODES) return NULL;
    g_header->tree_next_free = idx + 1;
    BSTnode *n = &g_tree_pool[idx];
    memset(n, 0, sizeof(BSTnode));
    n->used = 1;
    return n;
}

/* ---- shared memory init ---- */

void *user_store_shm_init(void) {
    size_t list_bytes = USER_STORE_POOL_NODES * sizeof(ListNode);
    size_t tree_bytes = USER_STORE_POOL_NODES * sizeof(BSTnode);
    size_t total = sizeof(shm_header_t) + list_bytes + tree_bytes;

    g_shm_base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shm_base == MAP_FAILED) {
        log_error("user store: mmap failed");
        return NULL;
    }

    g_header = (shm_header_t *)g_shm_base;
    memset(g_header, 0, sizeof(*g_header));

    /* init process-shared mutex */
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&g_header->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    /* pool pointers */
    g_list_pool = (ListNode *)((char *)g_shm_base + sizeof(shm_header_t));
    g_tree_pool = (BSTnode *)((char *)g_shm_base + sizeof(shm_header_t) + list_bytes);

    /* init NIL sentinel (slot 0 in tree pool) */
    g_nil = tree_alloc(); /* uses slot 0 */
    g_nil->color = RBT_BLACK;
    g_nil->left_off  = OFF_T(g_nil);
    g_nil->right_off = OFF_T(g_nil);
    g_nil->parent_off = OFF_T(g_nil);
    g_nil->user_off = 0;

    g_header->nil_off = OFF_T(g_nil);

    /* init BST wrapper */
    g_header->root_off = OFF_T(g_nil);
    g_user_bst.root_off = OFF_T(g_nil);
    g_user_bst.nil_off  = OFF_T(g_nil);
    g_user_bst.size = 0;

    /* linked list head (use slot 0 in list pool, empty sentinel) */
    ListNode *head = list_alloc(); /* uses slot 0 */
    head->next_off = 0;
    g_header->head_off = OFF_L(head);

    /* next free starts after sentinel */
    /* list_next_free already bumped to 1 by list_alloc */

    log_info("user store: shared memory initialized");
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "user store: shm base=%p, total=%.1f MB, max=%d nodes",
                 g_shm_base, (double)total / (1024*1024),
                 USER_STORE_POOL_NODES);
        log_info(msg);
    }

    return g_shm_base;
}

/* legacy wrapper for standalone modes */
int user_store_load_csv(const char *path) {
    if (g_shm_base == NULL) {
        void *base = user_store_shm_init();
        if (base == NULL) return -1;
    }
    user_store_shm_load_csv(g_shm_base, path);
    return 0;
}

void user_store_shm_load_csv(void *base, const char *path) {
    FILE *fp;
    char line[512];
    int count = 0;

    if (base == NULL || path == NULL) return;

    /* set globals (workers call this after fork, base is same for all) */
    g_shm_base = base;
    g_header = (shm_header_t *)base;
    size_t list_bytes = USER_STORE_POOL_NODES * sizeof(ListNode);
    g_list_pool = (ListNode *)((char *)base + sizeof(shm_header_t));
    g_tree_pool = (BSTnode *)((char *)base + sizeof(shm_header_t) + list_bytes);
    g_nil = TREE(g_header->nil_off);
    g_user_bst.root_off = g_header->root_off;
    g_user_bst.nil_off  = g_header->nil_off;

    fp = fopen(path, "r");
    if (fp == NULL) {
        log_error("user store: cannot open user file");
        return;
    }

    log_info("user store: loading data into shared memory...");

    while (fgets(line, sizeof(line), fp) != NULL) {
        ListNode *node;
        ElemType elem;

        remove_newline(line);
        if (line[0] == '\0' || (count == 0 &&
            (strncmp(line, "username", 8) == 0 ||
             strncmp(line, "name,", 5) == 0 ||
             strncmp(line, "baianai", 7) == 0))) {
            /* skip header lines and first-data-line checks */
            if (count == 0) {
                /* Try to detect if this is a header */
                if (strchr(line, ',') != NULL &&
                    (strstr(line, "name") || strstr(line, "username"))) {
                    count++;
                    continue;
                }
            }
        }

        if (parse_csv_line(line, &elem) != 0) continue;

        node = list_alloc();
        if (node == NULL) {
            log_error("user store: list pool exhausted");
            break;
        }
        node->data = elem;

        /* insert at head of linked list */
        ListNode *head = LIST(g_header->head_off);
        node->next_off = head->next_off;
        head->next_off = OFF_L(node);

        /* insert into RBT */
        {
            BSTnode *tn = tree_alloc();
            if (tn == NULL) {
                log_error("user store: tree pool exhausted");
                break;
            }
            tn->user_off = OFF_L(node);
            tn->left_off  = OFF_T(g_nil);
            tn->right_off = OFF_T(g_nil);
            tn->parent_off = OFF_T(g_nil);
            tn->color = RBT_RED;
            bst_insert(&g_user_bst, node); /* will use tn */
        }

        g_header->size++;
        count++;
    }

    fclose(fp);

    g_header->root_off = g_user_bst.root_off;  /* sync final state */
    g_header->size = count;

    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "user store: loaded %d users into shared memory", count);
        log_info(msg);
    }
}

/* ---- v1.6: auth check against CSV user data ---- */
int user_store_auth(const char *mobile, const char *password) {
    if (mobile == NULL || password == NULL || g_shm_base == NULL) return 0;
    ListNode *cur = LIST(g_header->head_off);
    cur = LIST(cur->next_off);
    while (cur != NULL) {
        if (cur->used &&
            strcmp(cur->data.mobile, mobile) == 0 &&
            strcmp(cur->data.password, password) == 0) {
            return 1;
        }
        cur = LIST(cur->next_off);
    }
    return 0;
}

/* ---- find (linked list traversal, offset-based) ---- */

ListNode *user_store_find(const char *name) {
    if (name == NULL || g_shm_base == NULL) return NULL;
    ListNode *cur = LIST(g_header->head_off);
    cur = LIST(cur->next_off); /* skip head sentinel */
    while (cur != NULL) {
        if (cur->used && strcmp(cur->data.name, name) == 0) return cur;
        cur = LIST(cur->next_off);
    }
    return NULL;
}

ListNode *user_store_find_with_steps(const char *name, int *steps, int verbose) {
    if (name == NULL || g_shm_base == NULL) return NULL;
    ListNode *cur = LIST(g_header->head_off);
    cur = LIST(cur->next_off);
    int s = 0;
    while (cur != NULL) {
        s++;
        if (cur->used && strcmp(cur->data.name, name) == 0) {
            if (steps) *steps = s;
            return cur;
        }
        if (verbose) {
            printf("  [%d] checking: %s\n", s, cur->data.name);
        }
        cur = LIST(cur->next_off);
    }
    if (steps) *steps = s;
    return NULL;
}

/* ---- add (with mutex) ---- */

int user_store_add(const char *csv_line) {
    ElemType elem;
    ListNode *node;
    BSTnode *tn;

    if (csv_line == NULL || g_shm_base == NULL) return -1;
    if (parse_csv_line((char *)csv_line, &elem) != 0) return -1;

    pthread_mutex_lock(&g_header->lock);

    /* check duplicate */
    if (user_store_find(elem.name) != NULL) {
        pthread_mutex_unlock(&g_header->lock);
        return 1; /* EXISTS */
    }

    node = list_alloc();
    if (node == NULL) {
        pthread_mutex_unlock(&g_header->lock);
        return -1;
    }
    node->data = elem;

    /* insert at head */
    ListNode *head = LIST(g_header->head_off);
    node->next_off = head->next_off;
    head->next_off = OFF_L(node);

    /* RBT insert */
    tn = tree_alloc();
    if (tn == NULL) {
        pthread_mutex_unlock(&g_header->lock);
        return -1;
    }
    tn->user_off = OFF_L(node);
    tn->left_off  = OFF_T(g_nil);
    tn->right_off = OFF_T(g_nil);
    tn->parent_off = OFF_T(g_nil);
    tn->color = RBT_RED;
    bst_insert(&g_user_bst, node);

    g_header->size++;
    g_header->root_off = g_user_bst.root_off;  /* sync to shared memory */
    pthread_mutex_unlock(&g_header->lock);
    return 0;
}

/* ---- delete (with mutex) ---- */

int user_store_delete(const char *name) {
    if (name == NULL || g_shm_base == NULL) return -1;

    pthread_mutex_lock(&g_header->lock);

    /* find in linked list and unlink */
    ListNode *prev = LIST(g_header->head_off);
    ListNode *cur = LIST(prev->next_off);
    int found = 0;
    while (cur != NULL) {
        if (cur->used && strcmp(cur->data.name, name) == 0) {
            prev->next_off = cur->next_off;
            cur->used = 0;
            found = 1;
            break;
        }
        prev = cur;
        cur = LIST(cur->next_off);
    }

    if (!found) {
        pthread_mutex_unlock(&g_header->lock);
        return -1;
    }

    /* remove from RBT */
    bst_delete(&g_user_bst, name);

    g_header->size--;
    g_header->root_off = g_user_bst.root_off;  /* sync to shared memory */
    pthread_mutex_unlock(&g_header->lock);
    return 0;
}

/* ---- free (unmap) ---- */

void user_store_free(void) {
    if (g_shm_base) {
        /* master sets size to 0; workers just unmap */
        if (g_header) g_header->size = 0;
        size_t total = sizeof(shm_header_t) +
                       USER_STORE_POOL_NODES * sizeof(ListNode) +
                       USER_STORE_POOL_NODES * sizeof(BSTnode);
        munmap(g_shm_base, total);
        g_shm_base = NULL;
        g_header = NULL;
    }
}

/* ---- BST index wrappers ---- */

void user_store_print_index(void) {
    bst_inorder(&g_user_bst);
}

void user_store_format_users(char *buf, int buf_size, int *total, int *offset) {
    bst_format_users(&g_user_bst, buf, buf_size, total, offset);
}

ListNode *user_store_find_index(const char *name) {
    return bst_find(&g_user_bst, name);
}

void user_store_compare_search_method(const char *name, int verbose) {
    int list_steps = 0, bst_steps = 0;
    ListPtr result_list, result_bst;
    int list_size = g_header ? g_header->size : 0;
    int i, continuous_count = 10000;
    struct timeval tv_start, tv_end;
    long list_usec, bst_usec;
    int dummy_steps;

    if (name == NULL) return;

    printf("========================================\n");
    printf("  Search Method Comparison\n");
    printf("========================================\n");
    printf("  Target user: %s\n", name);
    printf("  Total users in store: %d\n", list_size);
    if (list_size > 0) {
        printf("  BST height (ideal log2): %.1f\n",
               log(list_size) / log(2));
    }
    printf("----------------------------------------\n");

    printf("\n[Single Search]\n");
    if (verbose) printf("\n");

    result_list = user_store_find_with_steps(name, &list_steps, verbose);
    if (!verbose) {
        printf("  Linked list: %s in %d step(s)\n",
               result_list ? "FOUND" : "NOT FOUND", list_steps);
    } else {
        printf("    => Linked list: %s in %d step(s)\n",
               result_list ? "FOUND" : "NOT FOUND", list_steps);
    }

    if (verbose) printf("\n");

    result_bst = bst_find_with_steps(&g_user_bst, name, &bst_steps, verbose);
    if (!verbose) {
        printf("  BST index:   %s in %d step(s)\n",
               result_bst ? "FOUND" : "NOT FOUND", bst_steps);
    } else {
        printf("    => BST index: %s in %d step(s)\n",
               result_bst ? "FOUND" : "NOT FOUND", bst_steps);
    }

    if (bst_steps > 0) {
        printf("\n  => BST was %.1fx faster (single search)\n",
               (float)list_steps / (float)bst_steps);
    }

    printf("\n[Continuous Search Benchmark (%d iterations)]\n", continuous_count);

    gettimeofday(&tv_start, NULL);
    for (i = 0; i < continuous_count; i++)
        user_store_find_with_steps(name, &dummy_steps, 0);
    gettimeofday(&tv_end, NULL);
    list_usec = (tv_end.tv_sec - tv_start.tv_sec) * 1000000L +
                (tv_end.tv_usec - tv_start.tv_usec);

    gettimeofday(&tv_start, NULL);
    for (i = 0; i < continuous_count; i++)
        bst_find_with_steps(&g_user_bst, name, &dummy_steps, 0);
    gettimeofday(&tv_end, NULL);
    bst_usec = (tv_end.tv_sec - tv_start.tv_sec) * 1000000L +
               (tv_end.tv_usec - tv_start.tv_usec);

    printf("  Linked list: %ld.%03ld ms\n", list_usec / 1000, list_usec % 1000);
    printf("  BST index:   %ld.%03ld ms\n", bst_usec / 1000, bst_usec % 1000);
    if (bst_usec > 0) {
        printf("\n  => BST was %.1fx faster (continuous, %d searches)\n",
               (float)list_usec / (float)bst_usec, continuous_count);
    }
    printf("========================================\n");
    log_info("user store: search method comparison completed");
}

/* ================================================================
 * v1.4: user_store_search — RBT inorder with multi-field AND
 * ================================================================ */

static int str_contains(const char *text, const char *query) {
    const char *t, *q, *p;
    int tlen, qlen;
    if (!text || !query) return 0;
    tlen = (int)strlen(text);
    qlen = (int)strlen(query);
    if (qlen == 0) return 1;
    if (qlen > tlen) return 0;
    for (t = text; *t != '\0'; t++) {
        p = t; q = query;
        while (*p && *q && tolower((unsigned char)*p) == tolower((unsigned char)*q))
            { p++; q++; }
        if (*q == '\0') return 1;
    }
    return 0;
}

static int criteria_match(const ElemType *data, const search_criteria_t *c) {
    if (c->name[0] && !str_contains(data->name, c->name)) return 0;
    if (c->phone[0] && !str_contains(data->phone, c->phone)) return 0;
    if (c->mobile[0] && !str_contains(data->mobile, c->mobile)) return 0;
    if (c->email[0] && !str_contains(data->email, c->email)) return 0;
    return 1;
}

static void search_inorder(BSTnode *node, BSTnode *nil,
                           const search_criteria_t *criteria,
                           char *buf, int buf_size,
                           int *offset, int *match_count) {
    if (node == nil || node == NULL || *offset >= buf_size - 512) return;
    search_inorder(TREE(node->left_off), nil, criteria, buf, buf_size, offset, match_count);
    if (node->used && node->user_off) {
        ListNode *lu = LIST(node->user_off);
        if (lu && lu->used && criteria_match(&lu->data, criteria)) {
            (*match_count)++;
            int w = snprintf(buf + *offset, buf_size - *offset,
                "<div class=\"result-card\">\n"
                "  <h2>%s</h2>\n"
                "  <table>\n"
                "    <tr><td class=\"label\">Phone:</td><td>%s</td></tr>\n"
                "    <tr><td class=\"label\">Birth:</td><td>%s</td></tr>\n"
                "    <tr><td class=\"label\">Mobile:</td><td>%s</td></tr>\n"
                "    <tr><td class=\"label\">Email:</td><td>%s</td></tr>\n"
                "  </table>\n</div>\n",
                lu->data.name, lu->data.phone, lu->data.birthdate,
                lu->data.mobile, lu->data.email);
            if (w > 0 && w < buf_size - *offset) *offset += w;
        }
    }
    search_inorder(TREE(node->right_off), nil, criteria, buf, buf_size, offset, match_count);
}

static void criteria_summary(const search_criteria_t *c, char *buf, int size) {
    int pos = 0, first = 1;
    buf[0] = '\0';
    if (c->name[0])   { pos += snprintf(buf+pos, size-pos, "%sName: %s", first?"":", ", c->name); first=0; }
    if (c->phone[0])  { pos += snprintf(buf+pos, size-pos, "%sPhone: %s", first?"":", ", c->phone); first=0; }
    if (c->mobile[0]) { pos += snprintf(buf+pos, size-pos, "%sMobile: %s", first?"":", ", c->mobile); first=0; }
    if (c->email[0])  { pos += snprintf(buf+pos, size-pos, "%sEmail: %s", first?"":", ", c->email); first=0; }
    if (first) snprintf(buf, size, "(none)");
}

int user_store_search(const search_criteria_t *criteria, char *buf, int buf_size) {
    int offset = 0, match_count = 0, written;
    char summary[256];
    if (!criteria || !buf || buf_size <= 0 || !g_shm_base) return -1;

    criteria_summary(criteria, summary, sizeof(summary));

    written = snprintf(buf + offset, buf_size - offset,
        "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<link rel=\"icon\" href=\"/icon/favicon.ico\">\n"
        "<title>Search Results</title>\n"
        "<style>\n"
        ":root{--bg:#f5f5f5;--card-bg:#fff;--text:#333;--muted:#666;"
        "--border:#e0e0e0;--accent:#2563eb;}\n"
        "*{margin:0;padding:0;box-sizing:border-box;}\n"
        "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;"
        "background:var(--bg);color:var(--text);line-height:1.6;}\n"
        "header{background:linear-gradient(135deg,#1e293b 0%%,#334155 100%%);"
        "color:#fff;padding:32px 24px;text-align:center;}\n"
        "header h1{font-size:1.6rem;}\n"
        "header p{margin-top:8px;opacity:0.8;font-size:0.9rem;}\n"
        ".container{max-width:720px;margin:0 auto;padding:24px 20px;}\n"
        ".result-card{background:var(--card-bg);border:1px solid var(--border);"
        "border-radius:10px;padding:18px;margin-bottom:12px;"
        "box-shadow:0 1px 3px rgba(0,0,0,0.04);}\n"
        ".result-card h2{font-size:1.1rem;color:var(--accent);margin-bottom:8px;}\n"
        ".result-card table{width:100%%;border-collapse:collapse;}\n"
        ".result-card td{padding:4px 8px;font-size:0.9rem;}\n"
        ".result-card td.label{color:var(--muted);width:80px;}\n"
        ".no-results{text-align:center;padding:48px 20px;color:var(--muted);}\n"
        ".match-count{text-align:center;color:var(--muted);font-size:0.85rem;margin-top:16px;}\n"
        ".back-link{display:inline-block;margin-top:16px;padding:8px 20px;"
        "background:var(--accent);color:#fff;border-radius:6px;"
        "text-decoration:none;font-size:0.9rem;}\n"
        ".back-link:hover{opacity:0.9;}\n"
        "footer{text-align:center;padding:24px 20px;color:var(--muted);font-size:0.82rem;}\n"
        "</style>\n</head>\n<body>\n"
        "<header>\n"
        "  <h1>&#128269; Search Results</h1>\n"
        "  <p>Criteria: <strong>%s</strong></p>\n"
        "</header>\n<div class=\"container\">\n", summary);
    if (written < 0 || written >= buf_size - offset) return -1;
    offset += written;

    BSTnode *root = TREE(g_user_bst.root_off);
    search_inorder(root, g_nil, criteria, buf, buf_size, &offset, &match_count);

    if (match_count == 0) {
        written = snprintf(buf + offset, buf_size - offset,
            "<div class=\"no-results\">\n"
            "  <p style=\"font-size:1.2rem;\">&#128533;</p>\n"
            "  <p>No users found matching the criteria.</p>\n</div>\n");
        if (written > 0 && written < buf_size - offset) offset += written;
    }

    written = snprintf(buf + offset, buf_size - offset,
        "<p class=\"match-count\">%d match(es) found</p>\n"
        "<p style=\"text-align:center;\">"
        "<a class=\"back-link\" href=\"/\">&larr; Back to Search</a></p>\n"
        "</div>\n<footer>MiniWeb Server v1.4 &middot; RBT Index Search</footer>\n"
        "</body>\n</html>\n", match_count);
    if (written > 0 && written < buf_size - offset) offset += written;

    return match_count;
}
