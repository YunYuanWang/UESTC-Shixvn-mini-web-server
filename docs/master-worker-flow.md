# mini_web_server v1.0 Master-Worker 模式代码运行流程

本文档以 curl 发送 HTTP 请求的视角，从代码级别完整追踪 master-worker 模式的运行全流程。

---

## 第一阶段：服务器启动

### 1. 命令行入口 — `src/main.c`

```bash
./mini_web_server master conf/server.conf
```

进入 `main()` 函数，命中 master 分发分支（`src/main.c:561`）：

```c
if (argc == 3 && strcmp(argv[1], "master") == 0) {
```

按顺序执行以下步骤：

#### ① 加载配置

```c
server_config_t master_config;
memset(&master_config, 0, sizeof(master_config));
load_config(argv[2], &master_config);  // 解析 conf/server.conf
```

`load_config()`（`src/config.c`）逐行读取 `key=value`，填充结构体：

| 字段 | 值 | 说明 |
|---|---|---|
| `host` | `"127.0.0.1"` | 监听地址 |
| `port` | `8080` | 监听端口 |
| `www_root` | `"www"` | Web 根目录 |
| `user_file` | `"data/users.csv"` | 用户 CSV 路径 |
| `log_path` | `"logs/server.log"` | 日志路径 |
| `max_connections` | `256` | 最大连接数 |
| `max_request_bytes` | `4096` | 最大请求体 |
| `worker_processes` | `2` | **v1.0**: worker 进程数 |
| `worker_shutdown_timeout_ms` | `3000` | **v1.0**: 关闭超时 |

#### ② 初始化日志

```c
log_init(master_config.log_path);
// → fopen("logs/server.log", "a")
// → static FILE *log_file 指向日志文件
```

`src/log.c` 中 `log_init()` 以追加模式打开日志文件，文件指针保存在模块级静态变量 `log_file` 中。

#### ③ 加载用户数据（关键：在 fork 之前，仅此一次）

```c
user_store_load_csv(master_config.user_file);
// → 加载 data/users.csv (100,000 条)
```

`src/user_store.c` 中的 `user_store_load_csv()` 做两件事：

1. **构建链表**：逐行 `malloc` 出 `ListNode`，头插法插入全局链表 `head_node`
2. **构建红黑树索引**：每插入一个节点，同时调用 `bst_insert()` 构建 RBT 索引

内存布局：
```
head_node → ListNode("baianai",...) → ListNode("baianbao",...) → ...
user_bst  → RBT 根节点 → 各 BSTnode 指向对应的 ListNode
```

#### ④ 启动 master

```c
print_config(&master_config);
int ret = master_worker_run(&master_config);  // 进入核心
```

---

### 2. Master 进程生命周期 — `src/master_worker.c`

`master_worker_run()` 包含 master 进程的全部逻辑。

#### Phase 1：安装信号处理器

```c
// SIGINT → master_sigint_handler   (g_master_shutdown = 1)
struct sigaction sa;
sa.sa_handler = master_sigint_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sigaction(SIGINT, &sa, NULL);

// SIGCHLD → master_sigchld_handler
//   内部: while (waitpid(-1, &status, WNOHANG) > 0) { ... }
//   一次信号到达前可能有多个子进程退出，用循环全部回收
sa.sa_handler = master_sigchld_handler;
sa.sa_flags = SA_RESTART;
sigaction(SIGCHLD, &sa, NULL);

// SIGPIPE → SIG_IGN
signal(SIGPIPE, SIG_IGN);
```

#### Phase 2：创建监听 socket

```c
listen_fd = master_create_listen_socket(config->host, config->port);
```

`master_create_listen_socket()` 内部（`src/master_worker.c`）：

```c
listen_fd = socket(AF_INET, SOCK_STREAM, 0);             // 创建 TCP socket
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, ...);      // 端口复用
inet_pton(AF_INET, host, &server_addr.sin_addr);           // 解析地址
bind(listen_fd, (struct sockaddr*)&server_addr, ...);      // 绑定 127.0.0.1:8080
listen(listen_fd, SOMAXCONN);                               // 开始监听
```

内核状态：
```
listen_fd=4 → 内核监听队列，等待客户端 TCP 连接
```

#### Phase 3：fork worker 进程

```c
for (int i = 0; i < num_workers; i++) {  // num_workers = 2
    pid_t pid = fork();
```

`fork()` 是分水岭。子进程获得父进程地址空间的**完整副本**（Copy-on-Write, CoW）：

```
fork() 之前 (master 进程):
┌──────────────────────────────────────┐
│  master 地址空间                      │
│  ├─ head_node → 100,000 ListNodes    │ ← 堆: 链表
│  ├─ user_bst  → RBT 索引节点         │ ← 堆: 红黑树
│  ├─ log_file  → FILE*                │ ← stdio 缓冲区
│  ├─ listen_fd = 4                    │ ← 文件描述符表
│  ├─ g_shutdown = 0                   │ ← 数据段
│  └─ 其他全局/静态变量                  │
└──────────────────────────────────────┘

fork() 之后 (两个 worker 子进程):
┌──────────────────────────────────┐   ┌──────────────────────────────────┐
│  Worker-1 地址空间 (CoW 副本)      │   │  Worker-2 地址空间 (CoW 副本)      │
│  ├─ head_node → 100,000 ListNodes │   │  ├─ head_node → 100,000 ListNodes │
│  ├─ user_bst → RBT 索引          │   │  ├─ user_bst → RBT 索引          │
│  ├─ log_file → FILE* (继承)      │   │  ├─ log_file → FILE* (继承)      │
│  ├─ listen_fd = 4 (继承)         │   │  ├─ listen_fd = 4 (继承)         │
│  └─ PID = B                      │   │  └─ PID = C                      │
└──────────────────────────────────┘   └──────────────────────────────────┘
```

> **关键点**：100,000 条用户数据和 RBT 索引没有被重新构建——它们通过 `fork()` 的 CoW
> 机制被 worker 继承。只要 worker 只做只读查找（链表遍历 / RBT 搜索），物理内存页也共享，
> 不发生实际拷贝。POST/DELETE 等写操作才会触发 CoW 复制对应页面。

**子进程 (worker) 代码路径：**

```c
if (pid == 0) {
    // ==========================================
    //  CHILD: worker process
    // ==========================================

    // ① 重新打开日志——丢弃继承的 FILE*，获取独立的 stdio 缓冲区
    log_reopen(log_path);
```

`log_reopen()` 实现（`src/log.c`）：

```c
int log_reopen(const char *path) {
    if (log_file != NULL) {
        fclose(log_file);      // 关闭从 master 继承的 FILE*
        log_file = NULL;
    }
    log_file = fopen(path, "a");  // worker 自己打开独立的 FILE*
    return (log_file == NULL) ? -1 : 0;
}
```

> 为什么必须这样做？`FILE*` 结构包含用户态缓冲区（glibc 默认 8KB）。
> 如果多个进程共享从 master 继承的同一个 `FILE*`，各自 `fprintf` 写
> 入各自 CoW 后的缓冲副本，`fflush` 时内核虽然通过 O_APPEND 保证
> `write()` 原子性，但 FILE 内部状态（位置指针、缓冲区指针）会互相
> 覆盖。`log_reopen()` 让每个 worker 拥有完全独立的 `FILE*`。

```c
    // ② 重置信号——worker 不关心子进程
    signal(SIGCHLD, SIG_DFL);

    // ③ 记录启动日志
    log_info("[Worker] PID %d started, listen_fd=%d", getpid(), listen_fd);

    // ④ 进入 epoll 事件循环（永不返回，除非收到 SIGTERM/SIGINT）
    epoll_server_worker_run(listen_fd);

    // ⑤ worker 退出
    log_info("[Worker] PID %d shutting down", getpid());
    log_close();
    _exit(0);   // _exit 而非 exit——不刷新从 master 继承的 stdio 缓冲区
}
```

**父进程 (master) 代码路径：**

```c
    // ==========================================
    //  PARENT: track worker PID
    // ==========================================
    g_workers[g_num_workers++] = pid;
    log_info("[Master] forked worker %d, PID %d", i + 1, pid);
}
```

控制台输出：
```
[Master] 2 worker(s) started (PIDs: 111813 111814)
```

#### Phase 4：master 进入等待循环

```c
while (!g_master_shutdown) {
    pause();  // 阻塞，直到收到任何信号
}
```

`pause()` 使 master 进入可中断睡眠，**不消耗 CPU**。两种信号能唤醒：

| 信号 | 处理器行为 | pause 返回值 |
|---|---|---|
| SIGINT | `master_sigint_handler` → `g_master_shutdown = 1` | EINTR → 循环退出 |
| SIGCHLD | `master_sigchld_handler` → `waitpid()` 回收子进程 | EINTR → `g_master_shutdown` 仍为 0 → 继续循环 |

---

### 3. Worker 进入 epoll 事件循环 — `src/epoll_server.c`

#### 薄封装：`epoll_server_worker_run()`

```c
int epoll_server_worker_run(int listen_fd) {
    g_worker_mode       = 1;     // ← 关键标志位
    g_master_listen_fd  = listen_fd;
    g_shutdown          = 0;
    g_worker_shutdown   = 0;
    return epoll_server_run("0.0.0.0", 0);  // host/port 在 worker 模式被忽略
}
```

#### 进入 `epoll_server_run()` —— worker 模式的分支

**信号处理器安装（所有模式都执行）：**

```c
struct sigaction sa;
sa.sa_handler = sigint_handler;   // Ctrl-C → g_shutdown = 1
sigaction(SIGINT, &sa, NULL);
sa.sa_handler = sigterm_handler;  // Master 发来 → g_shutdown = 1, g_worker_shutdown = 1
sigaction(SIGTERM, &sa, NULL);
signal(SIGPIPE, SIG_IGN);
```

**Worker 模式跳过的步骤（`g_worker_mode = 1`）：**

```c
if (g_worker_mode) {
    listen_fd = g_master_listen_fd;  // 直接使用继承的 fd=4
    log_info("[Worker] using inherited listen_fd=%d", listen_fd);
} else {
    // socket() → setsockopt() → bind() → listen()
    // ↑ Worker 模式不执行
}
```

**所有模式都执行的步骤：**

```c
// 分配连接跟踪数组：1024 个 conn_t 槽位
connections = calloc(EPOLL_MAX_CONNS, sizeof(conn_t));

// 创建 epoll 实例
epfd = epoll_create1(0);

// 将监听 socket 加入 epoll 关注列表
ev.events   = EPOLLIN;
ev.data.ptr = NULL;   // NULL = 标记位，区分 listen_fd 和 client_fd
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
```

此时两个 worker 的内存和 fd 状态：

```
Worker-1 (PID B):
  进程地址空间:
    connections[0..1023]  fd 全部 = -1 (空闲)
    epfd = 5
    listen_fd = 4 (从 master 继承)
  内核:
    epoll 实例(epfd=5) 监控: fd=4 (listen_fd)

Worker-2 (PID C):
  同上（完全独立的副本）
```

两个 worker 都阻塞在：
```c
while (!g_shutdown) {
    nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 1000);
    // ↑ 1 秒超时，用于 keep-alive 空闲检测
}
```

内核保证：当新连接到达 `listen_fd=4` 时，**只唤醒一个** worker 的 `epoll_wait()`——这就是 Linux 内核的 accept 惊群防护。

---

## 第二阶段：curl 发送请求

### 场景 1：`curl http://127.0.0.1:8080/hello`

#### 内核层面

1. curl 发起 TCP 三次握手 → `127.0.0.1:8080`
2. 内核将新连接放入 `listen_fd=4` 的 accept 队列
3. 内核选择 **一个** worker（假设 Worker-1, PID B），唤醒其 `epoll_wait()`

#### Worker-1 代码路径（`src/epoll_server.c` 事件循环）

**① epoll_wait 返回**

```c
nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, 1000);
// nfds = 1, events[0].data.ptr == NULL, events[0].events = EPOLLIN
```

`ptr == NULL` → 这是 listen_fd 就绪（新连接到达）。

**② accept 新连接**

```c
if (ready_fd == listen_fd) {
    conn_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    // conn_fd = 6（内核分配的最小可用 fd）

    // 找到空闲槽位
    int slot = -1;
    for (int j = 0; j < EPOLL_MAX_CONNS; j++) {
        if (connections[j].fd == -1) { slot = j; break; }
    }
    // slot = 0

    // 填充 conn_t
    connections[0].fd            = 6;
    connections[0].recv_len      = 0;
    connections[0].request_count = 0;
    connections[0].last_activity = time(NULL);
    connections[0].client_port   = 54321;  // curl 临时端口
    inet_ntop(..., connections[0].client_ip, "127.0.0.1");

    // 加入 epoll，存储 conn_t 指针 → O(1) 事件查找
    ev.events   = EPOLLIN;
    ev.data.ptr = &connections[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, 6, &ev);
}
```

**③ curl 发送 HTTP 请求 → epoll_wait 再次返回**

```c
// events[i].data.ptr = &connections[0]
conn_t *conn = (conn_t *)events[i].data.ptr;  // O(1) 查找
n = recv(conn_fd, conn->recv_buf + conn->recv_len, RECV_BUF_SIZE - conn->recv_len - 1, 0);
```

`conn->recv_buf` 内容：
```
GET /hello HTTP/1.1\r\n
Host: 127.0.0.1:8080\r\n
User-Agent: curl/8.x\r\n
Accept: */*\r\n
\r\n
```

**④ 解析 HTTP 请求行**

```c
// 提取第一行
strncpy(line_copy, conn->recv_buf, sizeof(line_copy) - 1);
// 找到第一个 \r 或 \n → 截断

// 检查是否为已知 HTTP 方法
// "GET" ∈ {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", ...} → is_http = 1

method = line_copy;        // → "GET"
nl = strchr(line_copy, ' ');
*nl = '\0';
path = nl + 1;             // → "/hello"
// 去掉末尾的 " HTTP/1.1"
```

**⑤ 填充 `request_t`**

```c
request_t req;
memset(&req, 0, sizeof(req));
strncpy(req.method, "GET", sizeof(req.method) - 1);
strncpy(req.path, "/hello", sizeof(req.path) - 1);
// req.body = ""（GET 无 body）
```

**⑥ 调用 HTTP 路由处理**

```c
request_handler_process_http(&req, resp_buf, sizeof(resp_buf));
```

进入 `src/request_handler.c` → `request_handler_process_http()`：

```c
// 路由匹配:
if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/hello") == 0) {
    return build_http_response(output, size, 200, "OK", "Hello, Web!\n");
}
```

`build_http_response()` 构造 HTTP/1.1 响应：

```c
static int build_http_response(char *output, int size,
                                int status, const char *status_text,
                                const char *body) {
    int body_len = strlen(body);  // 12
    snprintf(output, size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        status, status_text, body_len, body);
}
// resp_buf:
// "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n"
// "Connection: keep-alive\r\n\r\nHello, Web!\n"
```

**⑦ v1.0 日志合并（一次日志调用包含全部关键信息）**

```c
// 提取状态码
resp_status = atoi(resp_buf + 9);  // "200"

// 单次日志调用——包含 PID + fd + path + status
snprintf(msg, sizeof(msg),
         "[EpollServer] fd=%d %s %s -> %d",
         ready_fd, method, path, resp_status);
log_info(msg);
```

实际日志输出：
```
[INFO] [PID 111813] [TID ...] [2026-07-18 10:00:00.123456] [EpollServer] fd=6 GET /hello -> 200
```

- **PID**: `111813`（Worker-1 的 PID，日志框架自动注入）
- **连接 fd**: `fd=6`
- **请求路径**: `GET /hello`
- **状态码**: `-> 200`
- 以上全部在一次 `fprintf` + `fflush` = 一次 `write()` 系统调用中完成

**⑧ 发送响应**

```c
sent = send(ready_fd, resp_buf, strlen(resp_buf), 0);
// curl 收到完整响应 → 终端打印 "Hello, Web!"
```

**⑨ keep-alive 处理**

```c
conn->request_count++;  // → 1

if (conn->request_count >= MAX_KEEP_ALIVE_REQUESTS) {
    // MAX_KEEP_ALIVE_REQUESTS = 100, 1 < 100 → 不触发
} else {
    // 保持连接：清空缓冲区，fd 留在 epoll 中等待下一个请求
    conn->recv_len = 0;
    memset(conn->recv_buf, 0, RECV_BUF_SIZE);
    log_info("[EpollServer] keep-alive: waiting for next request");
}
```

`fd=6` 留在 epoll 中，同一个 TCP 连接可以继续发送请求。

---

### 场景 2：`curl http://127.0.0.1:8080/users/ZhangSan`

假设内核将新连接分配给 Worker-2（PID C）。代码路径同场景 1，直到路由匹配阶段。

进入 `request_handler_process_http()`：

```c
// 路由匹配: GET /users/<name>
if (strcmp(req->method, "GET") == 0 && strncmp(req->path, "/users/", 7) == 0) {
    const char *name = req->path + 7;  // → "ZhangSan"
    ListNode *user = user_store_find(name);
```

`user_store_find("ZhangSan")`（`src/user_store.c`）：

```c
ListNode *user_store_find(const char *name) {
    ListPtr current = head_node.next;  // ← 从 master 继承的链表头
    while (current != NULL) {
        if (strcmp(current->data.name, name) == 0) {
            log_info("user store: user found");
            return current;  // FOUND
        }
        current = current->next;
    }
    log_info("user store: user not found");
    return NULL;  // NOT FOUND
}
```

- `head_node` 指向通过 `fork()` CoW 继承的 100,000 条用户数据
- 遍历的是**内存中的链表**，没有重新读取 `data/users.csv`
- 没有重新构建 RBT 索引

**找到用户的情况：**

```c
if (user != NULL) {
    format_user_info(user, body, sizeof(body));
    // body = "name: ZhangSan\npassword: xxx\nbirthdate: ...\nphone: ...\n..."
    return build_http_response(output, size, 200, "OK", body);
}
```

**未找到用户的情况：**

```c
snprintf(body, sizeof(body), "NOT_FOUND %s\n", name);
return build_http_response(output, size, 404, "NOT FOUND", body);
```

日志输出（Worker-2, PID C）：
```
[INFO] [PID 111814] [...] [EpollServer] fd=6 GET /users/ZhangSan -> 200
[INFO] [PID 111814] [...] [EpollServer] fd=6 GET /users/NonExistent -> 404
```

---

### 场景 3：`curl -X POST -d "TestUser,pass123,20000101,010-111,139123,test@t.com" http://127.0.0.1:8080/users`

#### 接收与解析

`recv_buf` 内容：
```
POST /users HTTP/1.1\r\n
Host: 127.0.0.1:8080\r\n
Content-Length: 58\r\n
Content-Type: application/x-www-form-urlencoded\r\n
\r\n
TestUser,pass123,20000101,010-111,139123,test@t.com
```

解析：

```c
// 第一行: method = "POST", path = "/users"

// 提取 body: \r\n\r\n 之后的内容
body_start = strstr(conn->recv_buf, "\r\n\r\n") + 4;
strncpy(req.body, body_start, sizeof(req.body) - 1);
// req.body = "TestUser,pass123,20000101,010-111,139123,test@t.com"
```

#### 路由处理

```c
// request_handler_process_http()
if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/users") == 0) {
    int ret = user_store_add(req->body);
```

`user_store_add()` 内部（`src/user_store.c`）：

```c
int user_store_add(const char *csv_line) {
    ElemType new_elem;

    // ① 解析 CSV 行 → ElemType{name, password, birthdate, phone, mobile, email}
    parse_csv_line(buffer, &new_elem);

    // ② 检查是否已存在（链表遍历——从 master 继承的数据）
    existing = user_store_find(new_elem.name);
    if (existing != NULL) { return -1; /* EXISTS */ }

    // ③ malloc 新节点，头插链表
    ListNode *node = malloc(sizeof(ListNode));
    node->data = new_elem;
    insert_head(node);  // head_node.next → node → 原链表...

    // ④ 插入 RBT 索引（可能会触发红黑树旋转 → 修改树结构）
    bst_insert(&user_bst, node);

    // ⑤ 追加写入磁盘 CSV 文件
    fp = fopen("data/users.csv", "a");
    fprintf(fp, "%s\n", csv_line);
    fclose(fp);

    return 0;  // ADDED
}
```

> ⚠️ 注意：这里的 `malloc`、`insert_head`、`bst_insert` 修改了**当前 worker 的
> CoW 副本**。由于 CoW 机制，当前 worker 会在对应内存页上获得独立的物理副本。
> 其他 worker 和 master 的链表不受影响。这在 v1.0 中是可接受的——后续版本可
> 以用共享内存（`mmap` + `MAP_SHARED`）实现跨 worker 数据同步。

日志输出：
```
[INFO] [PID ...] [...] [EpollServer] fd=6 POST /users -> 200
```

---

### 场景 4：并发 40 个 curl 请求

```bash
for i in $(seq 1 40); do
    curl -s http://127.0.0.1:8080/hello &
done
wait
```

#### 内核分配

40 个 TCP 连接到达 `listen_fd=4` 的 accept 队列。内核**大致均匀**地唤醒两个 worker（Linux 内核 accept 队列的 FIFO 唤醒策略）：

```
Worker-1 epoll_wait() 被唤醒 ~20 次
  → accept ~20 个连接
  → 在自己的 connections[1024] 数组中分配槽位
  → 处理 ~20 个请求

Worker-2 epoll_wait() 被唤醒 ~20 次
  → accept ~20 个连接
  → 在自己的 connections[1024] 数组中分配槽位（完全独立）
  → 处理 ~20 个请求
```

两个 worker 的 `connections[]` 数组是**完全独立**的内存——各自在
`epoll_server_run()` 中通过 `calloc` 分配。Worker-1 的
`connections[0].fd=6` 和 Worker-2 的 `connections[0].fd=6` 是
**不同的客户端连接**（各自的内核 fd 表独立）。

#### 日志表现

```
[INFO] [PID 111813] [...] fd=6 GET /hello -> 200    ← Worker-1
[INFO] [PID 111814] [...] fd=6 GET /hello -> 200    ← Worker-2
[INFO] [PID 111813] [...] fd=7 GET /hello -> 200    ← Worker-1
[INFO] [PID 111814] [...] fd=7 GET /hello -> 200    ← Worker-2
[INFO] [PID 111813] [...] fd=8 GET /hello -> 200    ← Worker-1
[INFO] [PID 111814] [...] fd=8 GET /hello -> 200    ← Worker-2
...
```

两个不同 PID 交替出现——证明两个 worker 都在处理请求。

---

### 场景 5：客户端隔离 — `/sleep/3000` + `/hello` 并发

```bash
# 终端 1: 慢请求
curl http://127.0.0.1:8080/sleep/3000 &

# 终端 2: 快请求（在慢请求处理期间发送）
curl http://127.0.0.1:8080/hello
```

#### 流程

1. `/sleep/3000` 请求到达，内核分配给 **Worker-1**
2. Worker-1 路由到 `GET /sleep/3000`：

```c
// request_handler_process_http()
if (strcmp(req->method, "GET") == 0 && strncmp(req->path, "/sleep/", 7) == 0) {
    int delay_ms = atoi(req->path + 7);  // 3000
    if (delay_ms > 5000) delay_ms = 5000;  // 上限 5 秒
    usleep((unsigned int)(delay_ms * 1000));  // ← 阻塞 3 秒
    // ...
}
```

Worker-1 的事件循环线程阻塞在 `usleep(3000000)` 中。在此期间，Worker-1
的 `epoll_wait()` 不会返回，无法处理该 worker 上其他连接的新数据（keep-alive 场景）。

3. `/hello` 请求到达，内核检测到 Worker-1 正忙（不在 `epoll_wait` 中），
将新连接分配给 **Worker-2**

4. Worker-2 立即 `accept` → 解析 `/hello` → 返回 "Hello, Web!" → **立即响应**

5. 3 秒后 Worker-1 的 `usleep` 返回，完成 `/sleep/3000` 的响应

**客户端隔离验证通过**：一个 worker 的慢请求不会阻塞另一个 worker。

---

## 第三阶段：优雅关闭

### 用户按 Ctrl-C → SIGINT

终端驱动向**前台进程组**中所有进程广播 SIGINT——master + 两个 worker 同时收到。

```
SIGINT → Worker-1: sigint_handler → g_shutdown = 1
SIGINT → Worker-2: sigint_handler → g_shutdown = 1
SIGINT → Master:   master_sigint_handler → g_master_shutdown = 1
```

### Worker 的关闭路径

**① epoll_wait 被打断：**

```c
nfds = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, timeout_ms);
if (nfds < 0) {
    if (errno == EINTR) {
        if (g_shutdown) break;  // ← 跳出 while(!g_shutdown) 事件循环
        continue;
    }
}
```

**② 关闭所有活跃客户端连接：**

```c
log_info("[EpollServer] shutting down...");

for (int i = 0; i < EPOLL_MAX_CONNS; i++) {
    if (connections[i].fd != -1) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, connections[i].fd, NULL);
        close(connections[i].fd);
        connections[i].fd = -1;
        g_active_conns--;
    }
}
```

**③ 清理 epoll 和连接数组：**

```c
// worker 模式不关闭 listen_fd（master 拥有它）
if (!g_worker_mode) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, listen_fd, NULL);
    close(listen_fd);
}
close(epfd);

// 日志
if (g_worker_mode) {
    snprintf(msg, ..., "[Worker] shutdown — served %d client(s) (SIGTERM)", clients_served);
} else {
    snprintf(msg, ..., "[EpollServer] server shutdown — served %d client(s) (SIGINT)", clients_served);
}
log_info(msg);

free(connections);
```

**④ 返回 master_worker.c 的子进程分支：**

```c
// epoll_server_worker_run() 返回了
log_info("[Worker] PID %d shutting down", getpid());
log_close();    // fclose worker 自己的 log_file
_exit(0);       // _exit(0) 而非 exit(0)
                // _exit 不刷新 stdio 缓冲区（避免和 master 冲突）
                // 不调用 atexit 处理函数
```

### Master 的关闭路径

**① pause() 返回，跳出等待循环：**

```c
while (!g_master_shutdown) {
    pause();  // SIGINT → g_master_shutdown=1 → 返回 EINTR → 循环退出
}
```

**② 关闭监听 socket：**

```c
close(listen_fd);  // 不再接受新连接
log_info("[Master] listen socket closed");
```

**③ 向所有 worker 发送 SIGTERM（兜底通知）：**

```c
master_kill_workers(SIGTERM);

// 内部:
for (int i = 0; i < g_num_workers; i++) {
    if (g_workers[i] > 0) {
        kill(g_workers[i], SIGTERM);
        log_info("[Master] sending SIGTERM to worker PID %d", g_workers[i]);
    }
}
```

Worker 可能已经自己退出了（进程组广播的 SIGINT 已触发关闭），但 master
仍然发送 SIGTERM 作为兜底。如果 worker 已退出，`kill()` 返回 -1 (ESRCH)，
master 记录日志但不报错。

**④ 等待 worker 退出（带超时）：**

```c
remaining = master_wait_for_workers(timeout_ms);  // 3000ms 超时
```

`master_wait_for_workers()` 内部——轮询循环：

```c
static int master_wait_for_workers(int timeout_ms) {
    struct timeval start, now;
    gettimeofday(&start, NULL);
    int remaining = /* 计数值 */;

    while (remaining > 0) {
        wpid = waitpid(-1, &status, WNOHANG);
        if (wpid > 0) {
            log_info("[Master] reaped worker PID %d", wpid);
            remaining--;  // 成功回收一个
        } else if (wpid == 0) {
            // 检查超时
            gettimeofday(&now, NULL);
            elapsed = (now.tv_sec - start.tv_sec) * 1000 + ...;
            if (elapsed >= timeout_ms) break;  // 超时，跳出
            usleep(50000);  // 50ms 后重试
        } else {
            // errno == ECHILD → 没有子进程了
            break;
        }
    }
    return remaining;  // 返回仍未回收的数量
}
```

**正常情况（worker 响应迅速）：**

```
[Master] sending SIGTERM to worker PID 111813
[Master] sending SIGTERM to worker PID 111814
[Master] reaped worker PID 111813      ← ~50ms 内回收
[Master] reaped worker PID 111814      ← ~50ms 内回收
[Master] all workers stopped
```

**异常情况（worker 卡在慢请求中，超过 3000ms）：**

```
[Master] waiting up to 3000 ms for workers to exit
... 3000ms ...
[Master] 1 worker(s) still alive after timeout, sending SIGKILL
kill(worker_PID, SIGKILL)           ← 强制杀死
usleep(100000)                       ← 等待 SIGKILL 生效
waitpid(worker_PID, NULL, 0)        ← 回收僵尸
[Master] all workers stopped
```

**⑤ 完成日志，返回：**

```c
log_info("[Master] all workers stopped");
log_info("[Master] shutdown complete");
return 0;
```

**⑥ 回到 main()：**

```c
int ret = master_worker_run(&master_config);  // 0
log_close();       // 关闭 master 的日志文件
user_store_free(); // 释放链表 + RBT（master 的副本）
return ret;        // 进程退出，返回码 0
```

---

## 完整时间线总结

```
时间    事件                                          PID
─────   ──────────────────────────────────────────  ──────
T+0.00  main(): load_config()                        111812
        log_init("logs/server.log")                  (master)
        user_store_load_csv("data/users.csv")
        → 100,000 条用户数据在堆上: 链表 + RBT

T+0.05  master_worker_run():                          111812
          socket() → bind() → listen(4)
          fork() → Worker-1                           111813
            ├─ log_reopen() → 独立 FILE*
            ├─ epoll_server_worker_run(4)
            │    calloc(connections[1024])
            │    epoll_create1() → epfd=5
            │    epoll_ctl(ADD, listen_fd=4)
            │    epoll_wait() ... ← 阻塞
          fork() → Worker-2                           111814
            └─ 同上

T+0.10  master: pause() ... ← 阻塞                   111812

T+5.00  curl /hello → Worker-1                       111813
           accept(fd=6) → recv → parse
           → request_handler_process_http()
           → build_http_response(200, "Hello, Web!")
           → log_info("fd=6 GET /hello -> 200")
           → send(fd=6, response)
           → keep-alive

T+5.10  curl /users/ZhangSan → Worker-2              111814
           accept(fd=6) → recv → parse
           → user_store_find("ZhangSan")
           → 遍历从 master 继承的链表 (CoW)
           → log_info("fd=6 GET /users/ZhangSan -> 200")

T+6.00  40 个并发 curl                                 111813
          内核均匀分配: ~20 给 Worker-1               111814
          ~20 给 Worker-2
          日志 PID 交替出现

T+10.00 Ctrl-C → SIGINT → 进程组广播
        Worker-1: g_shutdown=1 → 关闭客户端            111813
                  → log → _exit(0)
        Worker-2: g_shutdown=1 → 关闭客户端            111814
                  → log → _exit(0)
        Master: g_master_shutdown=1 → 跳出 pause       111812
                  → close(listen_fd)
                  → kill(SIGTERM, W1) kill(SIGTERM, W2)
                  → waitpid() 循环回收
                  → [Master] all workers stopped
                  → log_close() → user_store_free()
                  → exit(0)

T+10.20 进程退出，返回码 0。无僵尸进程。
```

---

## 关键设计决策

### 1. 用户数据 CoW 共享

`user_store_load_csv()` 在 master 中调用一次，在 `fork()` 之前。每个 worker
通过 CoW 获得一份逻辑副本。只读操作（`GET /users`、`GET /users/<name>`）不
触发实际物理拷贝。写操作（`POST /users`、`DELETE /users/<name>`）会触发当前
worker 的 CoW 页面复制，但不影响其他 worker。

### 2. 日志独立 FILE*

`log_reopen()` 解决 fork 后 `FILE*` 用户态缓冲区共享问题。每个 worker 拥有
独立的 `FILE*` 和独立的 stdio 缓冲区。底层 fd 通过 `O_APPEND` 保证每次
`write()` 系统调用原子追加。

### 3. 请求日志合并

每次 HTTP 请求仅产生一条日志，一次 `fprintf` + `fflush` = 一次 `write()` 系统
调用。日志包含 PID（框架注入）、连接 fd、请求路径和状态码。

### 4. listen_fd 生命周期

listen_fd 由 master 创建，worker 通过 fork 继承。worker 在关闭流程中**不**
关闭 listen_fd——每个 worker 关闭的只是自己 fd 表中的引用（不影响内核引用
计数）。master 在所有 worker 退出后才关闭 listen_fd。

### 5. 信号流

```
用户 Ctrl-C
  → 内核向进程组广播 SIGINT
  → Master + 所有 Worker 同时收到
  → Worker: 自己开始优雅关闭
  → Master: 关闭 listen_fd → 发送 SIGTERM 兜底 → waitpid 回收
  → 超时 → SIGKILL 强制终止
```

### 6. accept 惊群防护

Linux 内核（自 2.6 起）对 `epoll_wait()` + `accept()` 有内置的惊群防护：
多个进程 epoll 监听同一 fd 时，内核只唤醒一个进程。用户态无需额外互斥锁。

---

## 涉及文件一览

| 文件 | 角色 |
|---|---|
| `src/main.c` | 入口，master 分发分支 |
| `src/master_worker.c` | master 进程全部逻辑：socket / fork / 信号 / waitpid |
| `src/epoll_server.c` | worker epoll 事件循环，worker/standalone 双模式 |
| `src/request_handler.c` | HTTP 路由：/hello, /users, /sleep 等 |
| `src/user_store.c` | 用户链表 + add/find/delete（CoW 继承）|
| `src/user_index.c` | 红黑树 BST 索引（CoW 继承）|
| `src/config.c` | 配置解析（含 v1.0 新字段）|
| `src/log.c` | 日志模块：log_init / log_reopen / log_info / log_error |
| `include/master_worker.h` | master_worker_run() 声明 |
| `include/epoll_server.h` | epoll_server_worker_run() 声明 |
| `conf/server.conf` | 配置文件（含 worker_processes 等 v1.0 字段）|

---

> 文档版本: v1.0
> 最后更新: 2026-07-18
