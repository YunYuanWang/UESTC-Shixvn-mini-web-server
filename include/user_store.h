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

/* v1.1: format users into a buffer (BST inorder), safe for large datasets.
 * Writes up to buf_size bytes.  *total is set to the total user count,
 * *offset is advanced by the number of bytes written. */
void user_store_format_users(char *buf, int buf_size, int *total, int *offset);
ListNode *user_store_find_index(const char *name);
void user_store_compare_search_method(const char *name, int verbose);

/* v1.4: search criteria for multi-field AND search.
 * A field is active (used as filter) when its first character is non-null.
 * All active fields are combined with AND logic. */
typedef struct {
    char name[64];
    char phone[32];
    char email[64];
} search_criteria_t;

/* v1.4: RBT 中序遍历搜索用户（多字段 AND 匹配，大小写不敏感）。
 * 将匹配用户格式化为 HTML 写入 buf。返回匹配数量，出错返回 -1。 */
int user_store_search(const search_criteria_t *criteria, char *buf, int buf_size);

#endif
