#include "../include/user_store.h"
#include "../include/user_index.h"
#include "../include/log.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static ListNode head_node;
static BST user_bst;
static char csv_file_path[512];

static void remove_newline(char *text) {
    /*
    *   移除换行符
    */
    text[strcspn(text, "\r\n")] = '\0';
}

/*
 * insert_head — O(1) head insertion for fastest table building.
 * The linked list is unordered (random order); searches use linear
 * traversal.  Fast lookups are provided by the BST index.
 */
static void insert_head(ListPtr node) {
    node->next = head_node.next;
    head_node.next = node;
}

static int parse_csv_line(char *line, ElemType *elem) {
    char *token;
    int field;

    memset(elem, 0, sizeof(*elem));

    token = strtok(line, ",");
    for (field = 0; token != NULL; field++) {
        switch (field) {
        case 0:
            strncpy(elem->name, token, sizeof(elem->name) - 1);
            break;
        case 1:
            strncpy(elem->password, token, sizeof(elem->password) - 1);
            break;
        case 2:
            strncpy(elem->birthdate, token, sizeof(elem->birthdate) - 1);
            break;
        case 3:
            strncpy(elem->phone, token, sizeof(elem->phone) - 1);
            break;
        case 4:
            strncpy(elem->mobile, token, sizeof(elem->mobile) - 1);
            break;
        case 5:
            strncpy(elem->email, token, sizeof(elem->email) - 1);
            break;
        }
        token = strtok(NULL, ",");
    }

    if (field < 3) {
        return -1;
    }

    return 0;
}

int user_store_load_csv(const char *path) {
    FILE *fp;
    char line[512];
    int count = 0;

    if (path == NULL) {
        log_error("user store: csv path is NULL");
        return -1;
    }

    /* remember the path for subsequent save operations */
    strncpy(csv_file_path, path, sizeof(csv_file_path) - 1);
    csv_file_path[sizeof(csv_file_path) - 1] = '\0';

    /* initialize BST index */
    bst_init(&user_bst);

    fp = fopen(path, "r");
    if (fp == NULL) {
        log_error("user store: cannot open user file");
        return -1;
    }

    log_info("user store: loading data...");

    int first_line = 1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        ListPtr new_node;

        remove_newline(line);

        /* skip CSV header line (e.g. "username,password,phone") */
        if (first_line) {
            first_line = 0;
            if (strncmp(line, "username", 8) == 0) {
                continue;
            }
        }

        if (line[0] == '\0') {
            continue;
        }

        new_node = (ListPtr)malloc(sizeof(ListNode));
        if (new_node == NULL) {
            log_error("user store: malloc failed while loading users");
            fclose(fp);
            return -1;
        }

        if (parse_csv_line(line, &new_node->data) != 0) {
            free(new_node);
            continue;
        }

        new_node->next = NULL;
        insert_head(new_node);

        /* also insert into BST index */
        if (bst_insert(&user_bst, new_node) != 0) {
            log_error("user store: bst_insert failed");
        }

        count++;
    }

    fclose(fp);
    {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "user store: loaded %d users", count);
        log_info(buf);
    }
    return count;
}

ListNode *user_store_find(const char *name) {
    ListPtr current;

    if (name == NULL) {
        log_error("user store: find called with NULL name");
        return NULL;
    }

    current = head_node.next;
    while (current != NULL) {
        if (strcmp(current->data.name, name) == 0) {
            log_info("user store: user found");
            return current;
        }
        current = current->next;
    }

    log_info("user store: user not found");
    return NULL;
}

ListNode *user_store_find_with_steps(const char *name, int *steps, int verbose) {
    ListPtr current;
    int count = 0;

    if (name == NULL) {
        log_error("user store: find_with_steps called with NULL name");
        if (steps != NULL) {
            *steps = 0;
        }
        return NULL;
    }

    if (verbose) {
        printf("  [Linked list search path]\n");
    }

    current = head_node.next;
    while (current != NULL) {
        count++;
        if (verbose) {
            if (strcmp(current->data.name, name) == 0) {
                printf("    step %d: visit \"%s\" -> MATCH\n",
                       count, current->data.name);
            } else {
                printf("    step %d: visit \"%s\" -> not match, next\n",
                       count, current->data.name);
            }
        }

        if (strcmp(current->data.name, name) == 0) {
            if (steps != NULL) {
                *steps = count;
            }
            log_info("user store: user found with steps");
            return current;
        }
        current = current->next;
    }

    if (verbose) {
        printf("    (end of list, not found after %d steps)\n", count);
    }

    if (steps != NULL) {
        *steps = count;
    }
    log_info("user store: user not found with steps");
    return NULL;
}

int user_store_add(const char *csv_line) {
    ElemType new_elem;
    ListPtr existing;
    ListPtr node;
    char buffer[512];
    FILE *fp;

    if (csv_line == NULL) {
        log_error("user store: add called with NULL data");
        return -1;
    }

    strncpy(buffer, csv_line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    if (parse_csv_line(buffer, &new_elem) != 0) {
        log_error("user store: failed to parse add data");
        return -1;
    }

    existing = user_store_find(new_elem.name);
    if (existing != NULL) {
        log_error("user store: user already exists");
        return -1;
    }

    node = (ListPtr)malloc(sizeof(ListNode));
    if (node == NULL) {
        log_error("user store: malloc failed while adding user");
        return -1;
    }

    node->data = new_elem;
    node->next = NULL;
    insert_head(node);

    /* also insert into BST index */
    bst_insert(&user_bst, node);

    fp = fopen(csv_file_path[0] != '\0' ? csv_file_path : "data/users.csv", "a");
    if (fp != NULL) {
        fprintf(fp, "%s\n", csv_line);
        fclose(fp);
    }

    log_info("user store: user added");
    return 0;
}

static int user_store_save_csv(const char *path) {
    FILE *fp;
    ListPtr current;

    if (path == NULL) {
        log_error("user store: save csv path is NULL");
        return -1;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        log_error("user store: failed to open csv for writing");
        return -1;
    }

    current = head_node.next;
    while (current != NULL) {
        fprintf(fp, "%s,%s,%s,%s,%s,%s\n",
                current->data.name,
                current->data.password,
                current->data.birthdate,
                current->data.phone,
                current->data.mobile,
                current->data.email);
        current = current->next;
    }

    fclose(fp);
    return 0;
}

/*
 * user_store_delete: deletes a user by name.
 * IMPORTANT ordering: delete from BST index FIRST (only frees the tree
 * node, not the ListNode), THEN delete from linked list and free the
 * ListNode. This order prevents the BST from holding a dangling pointer.
 */
int user_store_delete(const char *name) {
    ListPtr prev;
    ListPtr current;

    if (name == NULL) {
        log_error("user store: delete called with NULL name");
        return -1;
    }

    /* Step 1: remove from BST index (frees only the tree node) */
    bst_delete(&user_bst, name);

    /* Step 2: remove from linked list and free the ListNode */
    prev = &head_node;
    current = head_node.next;
    while (current != NULL) {
        if (strcmp(current->data.name, name) == 0) {
            prev->next = current->next;
            free(current);
            log_info("user store: user deleted");
            user_store_save_csv(csv_file_path[0] != '\0' ? csv_file_path : "data/users.csv");
            return 0;
        }
        prev = current;
        current = current->next;
    }

    log_error("user store: user not found for deletion");
    return -1;
}

void user_store_free(void) {
    ListPtr current;
    ListPtr next;

    /* free BST index first (frees only tree nodes, not ListNodes) */
    bst_free(&user_bst);

    /* then free the linked list */
    current = head_node.next;
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }

    head_node.next = NULL;
}

/* ================================================================
 * BST index operations
 * ================================================================ */

void user_store_print_index(void) {
    bst_inorder(&user_bst);
}

/* v1.1: format users to buffer (safe, no pipe deadlock) */
void user_store_format_users(char *buf, int buf_size, int *total, int *offset) {
    if (!buf || buf_size <= 0 || !total || !offset) return;
    bst_format_users(&user_bst, buf, buf_size, total, offset);
}

ListNode *user_store_find_index(const char *name) {
    return bst_find(&user_bst, name);
}

void user_store_compare_search_method(const char *name, int verbose) {
    int list_steps = 0;
    int bst_steps = 0;
    ListPtr result_list;
    ListPtr result_bst;
    int list_size = 0;
    ListPtr cur;
    int i;
    int continuous_count = 10000;
    struct timeval tv_start, tv_end;
    long list_usec, bst_usec;
    int dummy_steps;

    if (name == NULL) {
        log_error("user store: compare_search_method called with NULL name");
        return;
    }

    /* count linked list size for reference */
    cur = head_node.next;
    while (cur != NULL) {
        list_size++;
        cur = cur->next;
    }

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

    /* --- Single search comparison --- */
    printf("\n[Single Search]\n");

    if (verbose) {
        printf("\n");
    }

    /* Linked list search with step counting */
    result_list = user_store_find_with_steps(name, &list_steps, verbose);
    if (!verbose) {
        if (result_list != NULL) {
            printf("  Linked list: FOUND in %d step(s)\n", list_steps);
        } else {
            printf("  Linked list: NOT FOUND after %d step(s)\n", list_steps);
        }
    } else {
        if (result_list != NULL) {
            printf("    => Linked list: FOUND in %d step(s)\n", list_steps);
        } else {
            printf("    => Linked list: NOT FOUND after %d step(s)\n", list_steps);
        }
    }

    if (verbose) {
        printf("\n");
    }

    /* BST search with step counting */
    result_bst = bst_find_with_steps(&user_bst, name, &bst_steps, verbose);
    if (!verbose) {
        if (result_bst != NULL) {
            printf("  BST index:   FOUND in %d step(s)\n", bst_steps);
        } else {
            printf("  BST index:   NOT FOUND after %d step(s)\n", bst_steps);
        }
    } else {
        if (result_bst != NULL) {
            printf("    => BST index: FOUND in %d step(s)\n", bst_steps);
        } else {
            printf("    => BST index: NOT FOUND after %d step(s)\n", bst_steps);
        }
    }

    if (bst_steps > 0) {
        printf("\n  => BST was %.1fx faster (single search)\n",
               (float)list_steps / (float)bst_steps);
    }

    /* --- Continuous search benchmark --- */
    printf("\n[Continuous Search Benchmark (%d iterations)]\n",
           continuous_count);

    /* Linked list continuous search */
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < continuous_count; i++) {
        user_store_find_with_steps(name, &dummy_steps, 0);
    }
    gettimeofday(&tv_end, NULL);
    list_usec = (tv_end.tv_sec - tv_start.tv_sec) * 1000000L +
                (tv_end.tv_usec - tv_start.tv_usec);

    /* BST continuous search */
    gettimeofday(&tv_start, NULL);
    for (i = 0; i < continuous_count; i++) {
        bst_find_with_steps(&user_bst, name, &dummy_steps, 0);
    }
    gettimeofday(&tv_end, NULL);
    bst_usec = (tv_end.tv_sec - tv_start.tv_sec) * 1000000L +
               (tv_end.tv_usec - tv_start.tv_usec);

    printf("  Linked list: %ld.%03ld ms\n",
           list_usec / 1000, list_usec % 1000);
    printf("  BST index:   %ld.%03ld ms\n",
           bst_usec / 1000, bst_usec % 1000);

    if (bst_usec > 0) {
        printf("\n  => BST was %.1fx faster (continuous, %d searches)\n",
               (float)list_usec / (float)bst_usec, continuous_count);
    } else {
        printf("\n  (BST time too small to measure meaningful ratio)\n");
    }

    printf("========================================\n");

    log_info("user store: search method comparison completed");
}

/* ================================================================
 * v1.4: user_store_search — RBT inorder traversal with multi-field
 *       AND filtering
 * ================================================================ */

/*
 * Case-insensitive substring match.
 * Returns 1 if 'query' is found as a substring of 'text' (case-insensitive),
 * 0 otherwise.  An empty query matches everything.
 * Works correctly with UTF-8 multi-byte sequences.
 */
static int str_contains(const char *text, const char *query) {
    const char *t, *q, *p;
    int tlen, qlen;

    if (!text || !query) return 0;

    tlen = (int)strlen(text);
    qlen = (int)strlen(query);

    if (qlen == 0) return 1;   /* empty query matches everything */
    if (qlen > tlen) return 0;

    for (t = text; *t != '\0'; t++) {
        p = t;
        q = query;
        while (*p != '\0' && *q != '\0' &&
               tolower((unsigned char)*p) == tolower((unsigned char)*q)) {
            p++;
            q++;
        }
        if (*q == '\0') return 1;  /* full query matched */
    }
    return 0;
}

/*
 * Check if a user matches ALL active criteria (AND logic).
 * A criterion is active if its first character is non-null.
 */
static int criteria_match(const ElemType *data,
                          const search_criteria_t *c) {
    if (c->name[0] != '\0' && !str_contains(data->name, c->name))
        return 0;
    if (c->phone[0] != '\0' && !str_contains(data->phone, c->phone))
        return 0;
    if (c->email[0] != '\0' && !str_contains(data->email, c->email))
        return 0;
    return 1;
}

/*
 * Recursive RBT inorder traversal that filters by criteria and writes HTML.
 */
static void search_inorder(BSTnode *node, BSTnode *nil,
                           const search_criteria_t *criteria,
                           char *buf, int buf_size,
                           int *offset, int *match_count) {
    int written;

    if (node == nil || node == NULL || *offset >= buf_size - 512) {
        return;
    }

    /* left subtree */
    search_inorder(node->left, nil, criteria,
                   buf, buf_size, offset, match_count);

    /* check this node against all active criteria (AND) */
    if (node->user != NULL && criteria_match(&node->user->data, criteria)) {
        (*match_count)++;

        written = snprintf(buf + *offset, buf_size - *offset,
            "<div class=\"result-card\">\n"
            "  <h2>%s</h2>\n"
            "  <table>\n"
            "    <tr><td class=\"label\">Phone:</td>"
                     "<td>%s</td></tr>\n"
            "    <tr><td class=\"label\">Birth:</td>"
                     "<td>%s</td></tr>\n"
            "    <tr><td class=\"label\">Mobile:</td>"
                     "<td>%s</td></tr>\n"
            "    <tr><td class=\"label\">Email:</td>"
                     "<td>%s</td></tr>\n"
            "  </table>\n"
            "</div>\n",
            node->user->data.name,
            node->user->data.phone,
            node->user->data.birthdate,
            node->user->data.mobile,
            node->user->data.email);
        if (written > 0 && written < buf_size - *offset) {
            *offset += written;
        }
    }

    /* right subtree */
    search_inorder(node->right, nil, criteria,
                   buf, buf_size, offset, match_count);
}

/*
 * Build a human-readable summary of active search criteria.
 */
static void criteria_summary(const search_criteria_t *c, char *buf, int size) {
    int pos = 0, first = 1;
    buf[0] = '\0';

    if (c->name[0] != '\0') {
        pos += snprintf(buf + pos, size - pos,
                        "%sName: %s", first ? "" : ", ", c->name);
        first = 0;
    }
    if (c->phone[0] != '\0') {
        pos += snprintf(buf + pos, size - pos,
                        "%sPhone: %s", first ? "" : ", ", c->phone);
        first = 0;
    }
    if (c->email[0] != '\0') {
        pos += snprintf(buf + pos, size - pos,
                        "%sEmail: %s", first ? "" : ", ", c->email);
        first = 0;
    }
    if (first) {
        snprintf(buf, size, "(none)");
    }
}

int user_store_search(const search_criteria_t *criteria,
                      char *buf, int buf_size) {
    int offset = 0;
    int match_count = 0;
    int written;
    char summary[256];

    if (criteria == NULL || buf == NULL || buf_size <= 0) {
        return -1;
    }

    criteria_summary(criteria, summary, sizeof(summary));

    /* build HTML page header */
    written = snprintf(buf + offset, buf_size - offset,
        "<!DOCTYPE html>\n"
        "<html lang=\"zh-CN\">\n<head>\n"
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
        ".match-count{text-align:center;color:var(--muted);font-size:0.85rem;"
        "margin-top:16px;}\n"
        ".back-link{display:inline-block;margin-top:16px;padding:8px 20px;"
        "background:var(--accent);color:#fff;border-radius:6px;"
        "text-decoration:none;font-size:0.9rem;}\n"
        ".back-link:hover{opacity:0.9;}\n"
        "footer{text-align:center;padding:24px 20px;color:var(--muted);"
        "font-size:0.82rem;}\n"
        "</style>\n"
        "</head>\n<body>\n"
        "<header>\n"
        "  <h1>&#128269; Search Results</h1>\n"
        "  <p>Criteria: <strong>%s</strong></p>\n"
        "</header>\n"
        "<div class=\"container\">\n",
        summary);
    if (written < 0 || written >= buf_size - offset) return -1;
    offset += written;

    /* RBT inorder traversal with multi-criteria filtering */
    search_inorder(user_bst.root, &user_bst.nil, criteria,
                   buf, buf_size, &offset, &match_count);

    /* no matches message */
    if (match_count == 0) {
        written = snprintf(buf + offset, buf_size - offset,
            "<div class=\"no-results\">\n"
            "  <p style=\"font-size:1.2rem;\">&#128533;</p>\n"
            "  <p>No users found matching the criteria.</p>\n"
            "</div>\n");
        if (written > 0 && written < buf_size - offset) {
            offset += written;
        }
    }

    /* footer with match count and back link */
    written = snprintf(buf + offset, buf_size - offset,
        "<p class=\"match-count\">%d match(es) found</p>\n"
        "<p style=\"text-align:center;\">"
        "<a class=\"back-link\" href=\"/\">&larr; Back to Search</a></p>\n"
        "</div>\n"
        "<footer>MiniWeb Server v1.4 &middot; RBT Index Search</footer>\n"
        "</body>\n</html>\n",
        match_count);
    if (written > 0 && written < buf_size - offset) {
        offset += written;
    }

    return match_count;
}
