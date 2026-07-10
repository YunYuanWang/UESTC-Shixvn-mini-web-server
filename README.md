# Mini Web Server

一个用 C 语言编写的迷你 Web 服务器，用于学习 Linux 系统编程。

## 构建

```bash
make clean
make
```

编译生成两个可执行文件：
- `mini_web_server` — 主程序（服务器模式 / 用户管理 / 进程启动器）
- `request_worker` — 请求处理工作进程（由主程序通过 `execl` 启动）

## 使用方法

### 服务器模式

```bash
./mini_web_server conf/server.conf
```

从配置文件加载设置，初始化日志和用户数据，输出 HTTP hello 响应。

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

### 多进程请求处理（V0.4）

```bash
./mini_web_server process
```

扫描 `requests/` 目录中所有 `.req` 文件，为每个请求通过 `fork() + execl()` 启动独立的 `request_worker` 子进程。
子进程处理请求并将结果写入 `outputs/<name>.out`，父进程通过 `waitpid` 等待所有子进程结束并记录结果。

**架构:**

```
mini_web_server process (父进程)
  ├── fork + execl → request_worker (子进程 1)
  ├── fork + execl → request_worker (子进程 2)
  └── fork + execl → request_worker (子进程 N)
```

**同步机制:** System V 信号量（参照 sday04 经典三信号量设计），保护多进程并发写入同一日志文件。

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

日志格式：每条日志必定包含进程 PID，可追溯到具体进程：

```
[LEVEL] [PID N] [YYYY-MM-DD HH:MM:SS.xxxxxx] message
```

示例：

```
[INFO] [PID 12345] [2026-07-09 12:00:00.123456] ========================================
[INFO] [PID 12345] [2026-07-09 12:00:00.234567]   Parent PID: 12345
[INFO] [PID 12345] [2026-07-09 12:00:00.345678] ========================================
[INFO] [PID 12346] [2026-07-09 12:00:01.123456] ====== Child 12346 started [hello.req] ======
[INFO] [PID 12346] [2026-07-09 12:00:01.234567] [Worker] processing request
[INFO] [PID 12346] [2026-07-09 12:00:01.345678] [Worker] request processed
```

- `log_info(msg)` / `log_error(msg)` — 自动使用当前进程 PID
- `log_info_pid(pid, msg)` / `log_error_pid(pid, msg)` — 显式指定 PID（父进程记录子进程时使用）
- `====` 横幅醒目标记父进程启动和各子进程创建

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
bash tests/test_day04.sh   # 多进程请求处理（全部 req 命令覆盖）
```
