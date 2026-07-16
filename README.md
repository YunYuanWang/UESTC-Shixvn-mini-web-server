# Mini Web Server

一个用 C 语言编写的迷你 Web 服务器，用于学习 Linux 系统编程。

## 构建

```bash
make clean
make
```

编译生成可执行文件：
- `mini_web_server` — 主程序（TCP 服务器模式 / 多进程 fork 模式 / 多线程 thread 模式 / 线程池 pool 模式 / select I/O 多路复用模式 / epoll I/O 多路复用模式 / 用户管理 / 多线程请求处理）
- `EpollServer` — 独立 epoll TCP/HTTP 服务器二进制文件（v0.10 新增）
- `epoll_client` — TCP 测试客户端（v0.10 新增）
- `request_worker` — 请求处理工作进程（遗留独立二进制，由旧版 fork+exec 模式使用）

## 使用方法

### 服务器模式 (TCP/HTTP)

```bash
./mini_web_server conf/server.conf
```

从配置文件加载设置，初始化日志和用户数据，启动 TCP 服务器监听 `host:port`（默认 `127.0.0.1:8080`），循环接受 HTTP 连接，处理请求并返回 HTTP/1.1 响应，按 Ctrl-C 退出。

### 多进程 fork 模式

```bash
./mini_web_server fork
```

父进程监听 127.0.0.1:8080，每个客户端连接 fork 一个子进程处理，支持并发请求。处理 MAX_CLIENTS (5) 个连接后自动退出。

使用 curl 测试：

```bash
# hello 端点
curl http://127.0.0.1:8080/hello
# → HTTP/1.1 200 OK
# → Hello, Web!

# 按名称查找用户
curl http://127.0.0.1:8080/users/ZhangSan
# → HTTP/1.1 200 OK (含全部用户字段)
# → 或 HTTP/1.1 404 NOT FOUND (用户不存在)

# 未知路径
curl http://127.0.0.1:8080/not-exist
# → HTTP/1.1 404 NOT FOUND

# 添加用户
curl -X POST -d "name,password,20000101,010-11111111,13900000000,test@test.com" http://127.0.0.1:8080/users

# 删除用户
curl -X DELETE http://127.0.0.1:8080/users/name
```

**注意:** conf/server.conf 模式下服务器循环处理连接，按 Ctrl-C 退出。fork 模式与 pool 模式下处理 MAX_CLIENTS 个连接后自动退出。如需再次测试，需重新启动服务器。

### 线程池 Pool 模式 (多线程 TCP/HTTP 服务器)

```bash
./mini_web_server pool
```

主线程读取 `conf/server.conf`，创建监听套接字（127.0.0.1:8080），启动固定大小的线程池（4 个 worker 线程），循环 `accept()` 客户端连接，将每个 `client_fd` 作为任务加入工作队列。worker 线程从队列中取出任务，调用已有的 HTTP 请求处理函数，处理完毕后关闭 `client_fd`。队列为空时 worker 线程阻塞等待。达到 MAX_CLIENTS (5) 后停止接收新连接，关闭线程池，唤醒所有 worker，等待所有线程退出。

**架构:**

```
./mini_web_server pool (主线程)
  │
  ├─ 创建监听套接字 (127.0.0.1:8080)
  ├─ 启动线程池 (4 个 worker 线程阻塞等待)
  ├─ accept() 循环（最多 MAX_CLIENTS 次）
  │     ├── Client-1 → 入队 → Worker-N: request_handler_handle_connection() → close(fd)
  │     ├── Client-2 → 入队 → Worker-M: request_handler_handle_connection() → close(fd)
  │     ├── ...
  │     └── Client-5 → 入队 → Worker-K: request_handler_handle_connection() → close(fd)
  ├─ 达到 MAX_CLIENTS → close(listen_fd)
  ├─ thread_pool_shutdown() → 唤醒所有 worker
  ├─ thread_pool_destroy() → pthread_join 所有 worker
  └─ 输出统计（accepted / processed / errors）→ 退出
```

**同步机制（POSIX 风格，参考 os_course）:**

| 原语 | 用途 |
|---|---|
| `pthread_mutex_t mutex` | 互斥量保护任务队列和 shutdown 标志 |
| `pthread_cond_t not_empty` | 条件变量，队列非空时唤醒等待的 worker 线程 |
| `pthread_cond_t not_full` | 条件变量，队列有空间时唤醒等待的 enqueuer |
| `pthread_mutex_t stats_mutex` | 互斥量保护统计数据（已处理数 / 错误数） |

**对比 fork 模式和 pool 模式:**

| 特性 | fork 模式 | pool 模式 |
|---|---|---|
| 并发模型 | 多进程（fork per connection） | 多线程（固定线程池） |
| 资源开销 | 每个连接一个进程 | 固定数量线程复用 |
| 地址空间 | 独立地址空间 | 共享地址空间 |
| 同步原语 | SIGCHLD + waitpid | mutex + condition variable |
| 适用场景 | 隔离性要求高 | 高并发低延迟 |

使用 curl 测试：

```bash
# hello 端点
curl http://127.0.0.1:8080/hello
# → HTTP/1.1 200 OK
# → Hello, Web!

# 支持所有与 fork 模式相同的 HTTP 端点
```

**日志示例:**

```
[INFO] [PID 607365] [TID 123460843988672] [2026-07-14 10:10:16.130854] [Worker-1] request: GET /hello
[INFO] [PID 607365] [TID 123460843988672] [2026-07-14 10:10:16.130862] [Worker-1] response: GET /hello -> 200
```

### select I/O 多路复用模式

```bash
./mini_web_server select <ip> <port>
# 例如: ./mini_web_server select 127.0.0.3 8888
```

单线程事件驱动服务器，使用 `select()` 系统调用同时监听多个文件描述符（监听 socket + 所有客户端连接）。当某个 fd 就绪时，读取 HTTP 请求、调用已有处理函数、发送响应、关闭连接。按 Ctrl-C 退出。

**架构:**

```
./mini_web_server select 127.0.0.1 8080 (主线程)
  │
  ├─ socket() → bind() → listen()
  ├─ FD_ZERO → FD_SET(listen_fd)
  └─ while !shutdown:
       ├─ select(max_fd+1, &read_set, NULL, NULL, NULL)
       └─ for each ready fd:
            ├─ fd == listen_fd  → accept() → FD_SET(client_fd)
            └─ fd == client_fd  → recv() → 解析 → 处理 → send() → close() → FD_CLR
```

**特性:**
- 单线程，无锁，无上下文切换开销
- 通过 `fd_set` 同时管理多达 1024 个连接
- 请求处理是同步的——慢请求会阻塞事件循环（适合快速响应的场景）

### epoll I/O 多路复用模式 (v0.10 新增)

```bash
# 方式一: 通过 mini_web_server 调度
./mini_web_server epoll <ip> <port>
# 例如: ./mini_web_server epoll 127.0.0.3 8888

# 方式二: 直接运行独立 EpollServer 二进制文件
./EpollServer <ip> <port>
# 例如: ./EpollServer 127.0.0.3 8888
```

单线程事件驱动服务器，使用 `epoll_create1()` / `epoll_ctl()` / `epoll_wait()` 系统调用同时监听多个文件描述符（监听 socket + 所有客户端连接）。当某个 fd 就绪时，epoll_wait 以 O(1) 复杂度返回就绪事件列表（无需像 select 那样 O(n) 扫描全部 fd）。

**架构:**

```
./EpollServer 127.0.0.1 8080 (单线程 epoll 事件循环)
  │
  ├─ socket() → bind() → listen()
  ├─ epoll_create1(0)
  ├─ epoll_ctl(EPOLL_CTL_ADD, listen_fd, EPOLLIN)
  └─ while !shutdown:
       ├─ epoll_wait(epfd, events, MAX_EVENTS, 1000ms)
       └─ for each ready event:
            ├─ fd == listen_fd  → accept() → epoll_ctl(EPOLL_CTL_ADD, client_fd)
            │                    → printf("[+] client connected (total: N)")
            └─ fd == client_fd  → recv() → 解析 HTTP → 处理 → send()
                                → printf("[client X] GET /hello -> 200")
                                → keep-alive 或 epoll_ctl(DEL) + close()
                                → printf("[-] client disconnected (total: N)")
```

**特性:**
- 使用 epoll 替代 select，无 FD_SETSIZE 限制（系统级 fd 限制）
- O(1) 就绪事件交付，高并发下性能远优于 select 的 O(n) 扫描
- Level-Triggered (LT) 模式，与 select 语义一致，简单可靠
- 实时 stdout 输出：连接数、客户端地址、客户端类型、请求消息
- 支持 HTTP/1.1 keep-alive（空闲超时 5s，最大 100 请求/连接）
- 支持原始 TCP 消息回显（非 HTTP 消息原样回显）

### TCP 测试客户端 (v0.10 新增)

```bash
./epoll_client <ip> <port>
# 例如: ./epoll_client 127.0.0.3 8888
```

连接到 EpollServer，从 stdin 读取消息发送到服务器，接收并打印响应。输入 `quit` 退出客户端。

**同时启动 3 个客户端演示:**

```bash
# 终端 1: 启动 EpollServer
./EpollServer 127.0.0.3 8888

# 终端 2: 客户端 1
./epoll_client 127.0.0.3 8888

# 终端 3: 客户端 2
./epoll_client 127.0.0.3 8888

# 终端 4: 客户端 3
./epoll_client 127.0.0.3 8888
```

服务器端将实时显示每个客户端的连接、消息和断开事件。

**对比四种并发模式:**

**对比四种并发模式:**

| 特性 | fork 模式 | pool 模式 | select 模式 | epoll 模式 |
|---|---|---|---|---|
| 并发模型 | 多进程 (fork) | 线程池 (2→8) | 单线程 I/O 复用 | 单线程 I/O 复用 |
| I/O 机制 | 阻塞 accept | 阻塞 accept | select() | epoll_wait() |
| fd 上限 | 系统限制 | 系统限制 | FD_SETSIZE (1024) | 系统限制 |
| 事件复杂度 | — | — | O(n) 扫描 | O(1) 就绪交付 |
| 每连接开销 | 高（独立进程） | 中（共享线程） | 低（事件驱动） | 低（事件驱动） |
| 内存占用 | 独立地址空间 | 共享地址空间 | 单一地址空间 | 单一地址空间 |
| 慢请求影响 | 进程隔离 | 其他 worker 不受影响 | 阻塞整个事件循环 | 阻塞整个事件循环 |
| 适用场景 | 隔离性要求高 | 通用高并发 | 中等并发短连接 | 高并发海量连接 |

使用 curl 测试：

```bash
# 启动 select 服务器
./mini_web_server select 127.0.0.1 8080 &

# hello 端点
curl http://127.0.0.1:8080/hello
# → Hello, Web!

# 支持所有与 fork/pool 模式相同的 HTTP 端点
```

### 用户管理

```bash
./mini_web_server findusr <name>
./mini_web_server addusr <name,password,birthdate,phone,mobile,email>
./mini_web_server delusr <name>
./mini_web_server users index
./mini_web_server users find-index <name>
./mini_web_server users compare_search_method <name>
./mini_web_server users compare_search_method --verbose <name>
```

### 交互模式

```bash
./mini_web_server load <csv_path>
```

### 多线程请求处理

```bash
./mini_web_server process
```

父线程扫描 `requests/` 目录中所有 `.req` 文件，将任务路径加入共享请求队列，然后创建多个 worker 线程并行处理。
每个 worker 线程从队列中取出任务，处理请求并将结果写入 `outputs/<name>.out`，主线程通过 `pthread_join` 等待所有 worker 结束并记录结果。

**架构:**

```
mini_web_server process (主线程)
  │
  ├─ 扫描 requests/ → 入队到共享队列
  ├─ pthread_create → worker 线程 × 4
  │     ├── Worker-1: sem_wait → 出队 → 处理 → outputs/
  │     ├── Worker-2: sem_wait → 出队 → 处理 → outputs/
  │     ├── Worker-3: sem_wait → 出队 → 处理 → outputs/
  │     └── Worker-4: sem_wait → 出队 → 处理 → outputs/
  └─ pthread_join × 4 → 统计结果
```

**同步机制（POSIX 风格，参考 os_course）:**

| 原语 | 用途 |
|---|---|
| `sem_t tasks_sem` | 计数信号量，计数可用任务数，worker 通过 `sem_wait` 等待 |
| `pthread_mutex_t queue_mutex` | 互斥量保护请求队列 |
| `pthread_cond_t queue_cond` | 条件变量，队列非空时唤醒等待的 worker 线程 |
| `pthread_mutex_t stats_mutex` | 互斥量保护统计数据（已处理数 / 错误数） |
| `pthread_mutex_t log_mutex` | 互斥量保护日志写入 |

### 多进程 TCP 服务器 (fork 模式)

```bash
./mini_web_server fork
```

父进程创建监听套接字（127.0.0.1:8080），对每个客户端连接 `fork()` 一个子进程处理 HTTP 请求，处理完成后子进程退出。父进程通过 `SIGCHLD` 信号 + `waitpid(WNOHANG)` 回收僵尸进程，避免资源泄漏。

服务器在处理 MAX_CLIENTS (5) 个连接后自动退出，适用于自动化测试。

**架构:**

```
./mini_web_server fork (父进程)
  │
  ├─ 创建监听套接字 (127.0.0.1:8080)
  ├─ accept() 循环（最多 MAX_CLIENTS 次）
  │     ├── Client-1 → fork() → 子进程-1: request_handler_handle_connection() → exit
  │     ├── Client-2 → fork() → 子进程-2: request_handler_handle_connection() → exit
  │     ├── ...
  │     └── Client-5 → fork() → 子进程-5: request_handler_handle_connection() → exit
  ├─ SIGCHLD 处理器: waitpid(-1, &stat, WNOHANG) 回收所有子进程
  ├─ SIGPIPE 忽略: send() 返回 EPIPE 而非终止进程
  ├─ accept() EINTR 处理: 被信号打断时继续等待
  └─ 达到 MAX_CLIENTS → close(listen_fd) → 退出
```

**信号处理:**

| 信号 | 处理方式 | 说明 |
|---|---|---|
| SIGCHLD | `waitpid(-1, &stat, WNOHANG)` 循环回收 | 避免僵尸进程 |
| SIGPIPE | `SIG_IGN` 忽略 | 客户端异常断开时 send() 返回 EPIPE，不会终止进程 |
| EINTR | `accept()` 返回 -1 且 errno==EINTR 时 continue | 被 SIGCHLD 打断后继续等待连接 |

**并发测试:**

```bash
# 启动 fork 服务器
./mini_web_server fork &

# 发送 5 个并发请求
for i in 1 2 3 4 5; do
  curl -s http://127.0.0.1:8080/hello &
done
wait
```

## 请求文件格式

`requests/` 目录下的 `.req` 文件，文件名与输出文件对应（`<name>.req` → `outputs/<name>.out`）。

### 支持的命令一览

| 请求文件内容 | 对应 CLI 命令 | 说明 |
|---|---|---|
| `GET /hello` | — | 返回 HTTP hello 响应 |
| `GET /user/<name>` | `findusr <name>` | 在链表中查找用户 |
| `GET /users` | `users index` | 列出全部用户（BST 中序遍历） |
| `GET /users/find-index/<name>` | `users find-index <name>` | 通过 BST 索引查找用户 |
| `GET /users/compare/<name>` | `users compare_search_method <name>` | 对比链表 vs BST 搜索性能 |
| `GET /users/compare-verbose/<name>` | `users compare_search_method --verbose <name>` | 详细对比（含遍历路径） |
| `POST /users` | `addusr <csv>` | 添加用户，CSV 数据放在第二行 |
| `DELETE /users/<name>` | `delusr <name>` | 删除用户 |

### GET 请求示例

```
GET /hello
```

```
GET /user/ZhangSan
```

```
GET /users
```

```
GET /users/find-index/ZhangSan
```

```
GET /users/compare/ZhangSan
```

```
GET /users/compare-verbose/ZhangSan
```

### POST 请求示例（两行：首行为方法+路径，第二行为 CSV 数据）

```
POST /users
TestUser,pass123,20000101,010-11111111,13900000000,test@test.com
```

### DELETE 请求示例

```
DELETE /users/TestUser
```

### 未匹配路径

```
GET /nonexistent
→ 404 Not Found: GET /nonexistent
```

## 输出文件格式

每个输出文件包含模拟的网络连接信息和响应体：

```
=== Connection Info ===
Client IP: 127.0.0.1
Client Port: 54321
Server: MiniWeb
=======================
<response body>
```

## 日志

所有日志统一输出到 `logs/server.log`。

日志格式：每条日志同时包含进程 PID 和线程 TID，可追溯到具体线程：

```
[LEVEL] [PID N] [TID N] [YYYY-MM-DD HH:MM:SS.xxxxxx] message
```

示例：

```
[INFO] [PID 2344043] [TID 132829928163136] [2026-07-10 11:48:13.804778] [ProcessServer] scanning requests (multi-thread mode)
[INFO] [PID 2344043] [TID 132829928163136] [2026-07-10 11:48:13.804815] [ProcessServer] enqueued: hello.req
[INFO] [PID 2344043] [TID 132829908293312] [2026-07-10 11:48:13.805374] [Worker-3] processing missing.req
[INFO] [PID 2344043] [TID 132829899900608] [2026-07-10 11:48:13.805545] [Worker-4] processing user_find.req
[INFO] [PID 2344043] [TID 132829928163136] [2026-07-10 11:48:13.806535] [ProcessServer] all workers done — processed: 3, errors: 0
```

- 同一进程内所有线程 PID 相同，通过 TID 区分：主线程 vs Worker-1/2/3/4
- `[ProcessServer]` 前缀 → 主线程日志；`[Worker-N]` 前缀 → worker 线程日志
- `log_info(msg)` / `log_error(msg)` — 自动使用当前 PID + TID
- `log_info_pid(pid, msg)` / `log_error_pid(pid, msg)` — 显式指定 PID（同时记录当前 TID）

## 目录结构

```
miniwebserver/
├── conf/           # 配置文件
├── data/           # CSV 用户数据
├── include/        # 头文件
├── logs/           # 日志输出
├── obj/            # 编译中间文件
├── outputs/        # 请求处理输出
├── requests/       # 请求文件
├── src/            # 源代码
├── tests/          # 测试脚本
├── www/            # Web 根目录
└── Makefile
```

## 测试

```bash
bash tests/test_day01.sh   # 配置加载与 HTTP 响应
bash tests/test_day02.sh   # 用户 CRUD 操作
bash tests/test_day03.sh   # BST 索引与搜索
bash tests/test_day04.sh   # 多线程请求处理（全部 req 命令覆盖）
bash tests/test_day06.sh   # TCP/HTTP 服务器（curl 模拟 HTTP 请求）
bash tests/test_day07.sh   # 多进程 TCP/HTTP 服务器（fork 模式，并发 curl 测试）
bash tests/test_day08.sh   # 多线程 TCP/HTTP 服务器（线程池模式，动态扩缩）
bash tests/test_day09.sh   # select I/O 多路复用服务器（select 事件驱动）
bash tests/test_day10.sh   # epoll I/O 多路复用服务器（epoll 事件驱动，v0.10）
```
