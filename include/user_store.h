#ifndef USER_STORE_H
#define USER_STORE_H

typedef struct UsrInfo {
    char name[64];
    char password[64];
    char birthdate[16];
    char phone[32];
    char mobile[32];
    char email[64];
} ElemType;

typedef struct node {
    ElemType data;
    struct node *next;
} ListNode, *ListPtr;

int user_store_load_csv(const char *path);
ListNode *user_store_find(const char *name);
ListNode *user_store_find_with_steps(const char *name, int *steps, int verbose);
int user_store_add(const char *csv_line);
int user_store_delete(const char *name);
void user_store_free(void);

/* BST index operations */
void user_store_print_index(void);
ListNode *user_store_find_index(const char *name);
void user_store_compare_search_method(const char *name, int verbose);

#endif
