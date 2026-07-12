#include "../include/user_store.h"
#include "../include/user_index.h"
#include "../include/log.h"
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
