# Mini Web Server

一个用 C 语言编写的迷你 Web 服务器，用于学习 Linux 系统编程。

## 构建

```bash
make clean
make
```

编译生成两个可执行文件：
- `mini_web_server` — 主程序（服务器模式 / 用户管理 / 多线程请求处理）
- `request_worker` — 请求处理工作进程（遗留独立二进制，由旧版 fork+exec 模式使用）

## 使用方法

### 服务器模式 (TCP/HTTP)

```bash
./mini_web_server conf/server.conf
```

从配置文件加载设置，初始化日志和用户数据，启动 TCP 服务器监听 `host:port`（默认 `127.0.0.1:8080`），接受一个 HTTP 连接，处理请求并返回 HTTP/1.1 响应，然后退出。

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

**注意:** 服务器每次只处理一个连接，处理完成后正常退出。如需再次测试，需重新启动服务器。

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
```
