/* ================================================================
 * mini_web_server v1.1 → v1.2 代码差异汇总
 *
 * 标记规则:
 *   [NEW]      — v1.2 新增文件，完整代码
 *   [MODIFIED] — v1.2 修改文件，只包含变更部分
 *   ... 省略   — 未变更代码用此标记
 *
 * 忽略: 编译产物 (.o, 二进制), 日志文件, 图片文件
 * ================================================================ */

/* ================================================================
 * [NEW] blog/ — 实验博客网站
 * 完整的多级目录静态网站，展示服务器文件服务能力
 * ================================================================ */

/*
 * blog/
 * ├── index.html          # 博客首页 — 版本演进、特性展示、性能数据
 * ├── about.html          # 架构介绍 — Epoll + Master-Worker 详解
 * ├── article.html        # 实验心得 — v0.7~v1.1 完整调试记录
 * ├── resume.html         # 关于作者 — 个人简介与项目经历
 * ├── css/style.css       # 成电蓝+银杏金主题样式 (496 行)
 * ├── js/main.js          # 交互脚本 (进度条、鼠标跟踪、点击特效)
 * ├── img/logo.png        # PNG 格式 → MIME: image/png
 * ├── img/banner.jpg      # JPEG 格式 → MIME: image/jpeg
 * ├── img/avatar.gif      # GIF 格式 → MIME: image/gif
 * ├── img/photo.jpg       # 作者照片
 * ├── icon/favicon.ico    # ICO 格式 → MIME: image/x-icon
 * └── README.md           # Blog 子 README
 */

/* ================================================================
 * [NEW] tests/test_blog.sh
 * 博客网站完整测试套件 (165 行)
 * 覆盖: MIME 类型 (7种), HTTP 状态码 (301/403/404/405), 多级目录
 * ================================================================ */

/* ================================================================
 * [MODIFIED] include/log.h — 日志系统 API 重新设计
 * ================================================================ */

// v1.1 原始:
// int log_init(const char *path);
// int log_reopen(const char *path);
// void log_info(const char *message);
// void log_error(const char *message);
// void log_info_pid(pid_t pid, const char *message);
// void log_error_pid(pid_t pid, const char *message);
// void log_close(void);

// v1.2 修改为:
typedef enum {
    LOG_DEBUG   = 0,
    LOG_INFO    = 1,
    LOG_WARNING = 2,
    LOG_ERROR   = 3,
    LOG_NONE    = 4
} log_level_t;

// ---- 初始化: 分别指定系统日志和访问日志路径 ----
int log_init(const char *system_log_path, const char *access_log_path,
             int max_lines, int max_roll_files);

// ---- 简化初始化 (兼容旧版单文件日志) ----
int log_init_single(const char *path);

// ---- 日志级别控制 ----
void log_set_level(log_level_t level);
log_level_t log_get_level(void);

// ---- 系统日志 (简单字符串, 兼容旧版) ----
void log_debug(const char *message);
void log_info(const char *message);     // 保留旧版签名
void log_warning(const char *message);
void log_error(const char *message);    // 保留旧版签名

// ---- 系统日志 (printf 风格, 新代码推荐) ----
void log_infof(const char *fmt, ...)    __attribute__((format(printf, 1, 2)));
void log_errorf(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));
void log_warningf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_debugf(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));

// ---- 访问日志 (结构化) ----
void log_access(const char *client_ip, const char *method,
                const char *path, int status, int bytes_sent);

// ---- log_reopen 不再需要 path 参数 (内部保存) ----
int log_reopen(void);              // v1.1: log_reopen(const char *path)

/* ================================================================
 * [MODIFIED] src/log.c — 日志系统完全重写
 * ================================================================ */

/*
 * v1.1: 单一日志文件, 仅 INFO/ERROR 两级, 无滚动
 * v1.2: 完全重写, 新增以下功能:
 *
 * 1. 双文件输出: 系统日志 (system_log) + 访问日志 (access_log)
 *    可分别指定路径, 也可共享同一文件
 *
 * 2. 四级日志: DEBUG, INFO, WARNING, ERROR
 *    log_set_level() 控制最低输出级别, 低于阈值的不写入
 *
 * 3. 两种调用风格:
 *    简单字符串: log_info("msg")        — 兼容旧版, 不做格式解析
 *    printf 风格: log_infof("val=%d", n) — 支持格式化, 新代码推荐
 *
 * 4. 日志滚动: 行数达到 max_lines 时自动滚动
 *    server.log → server.log.1 → server.log.2 → ... → 删除最旧
 *    滚动时: fclose → rename 链式移位 → fopen 新文件
 *
 * 5. 行数统计: 初始化时读取已有文件统计行数, 运行时内存计数
 *
 * 6. 线程安全: flockfile/funlockfile 保护每次写入
 *
 * 7. Post-fork 安全: log_reopen() 关闭父进程继承的 FILE*,
 *    重新打开独立句柄, 避免跨进程缓冲区污染
 *
 * 8. 双层缓冲区: 系统日志和访问日志独立计数, 独立触发滚动
 */

// ---- 系统日志格式 (v1.2) ----
// [2026-07-19 15:30:45.123456] [INFO ] [PID 12345] [TID 12345] message

// ---- 访问日志格式 (v1.2, 简化 combined) ----
// [19/Jul/2026:15:30:45 +0800] 127.0.0.1 GET /blog/index.html 200 9937

// ---- 日志滚动示例 ----
// logs/system.log        ← 当前写入
// logs/system.log.1      ← 上一次滚动
// logs/system.log.2      ← 更早
// ...
// logs/system.log.5      ← 最旧 (max_roll_files=5)

/* ================================================================
 * [MODIFIED] include/config.h — 新增日志配置字段
 * ================================================================ */

// v1.1 原始:
// typedef struct {
//     ...
//     char log_path[128];
//     int  max_connections;
//     ...
// } server_config_t;

// v1.2 新增字段:
    char system_log[128];          /* v1.2: separate system log path */
    char access_log[128];          /* v1.2: separate access log path */
    int  log_max_lines;            /* v1.2: rotate after N lines (0=no rotation) */
    int  log_max_roll_files;       /* v1.2: keep N old log files */

/* ================================================================
 * [MODIFIED] src/config.c — 解析新增日志配置字段
 * ================================================================ */

// ---- 新增 key 解析 ----
// else if (strcmp(key, "system_log") == 0) {
//     copy_text(config->system_log, sizeof(config->system_log), value);
// } else if (strcmp(key, "access_log") == 0) {
//     copy_text(config->access_log, sizeof(config->access_log), value);
// } else if (strcmp(key, "log_max_lines") == 0) {
//     config->log_max_lines = atoi(value);
// } else if (strcmp(key, "log_max_roll_files") == 0) {
//     config->log_max_roll_files = atoi(value);
// }

// ---- 新增默认值 ----
// if (config->log_max_lines <= 0)        config->log_max_lines = 10000;
// if (config->log_max_roll_files <= 0)   config->log_max_roll_files = 5;
// if (config->system_log[0] == '\0')     copy_text(..., config->log_path);  // 回退
// if (config->access_log[0] == '\0')     copy_text(..., config->log_path);  // 回退

// ---- print_config 新增字段 ----
// printf("system_log=%s\n",        config->system_log);
// printf("access_log=%s\n",        config->access_log);
// printf("log_max_lines=%d\n",     config->log_max_lines);
// printf("log_max_roll_files=%d\n",config->log_max_roll_files);

/* ================================================================
 * [MODIFIED] conf/server.conf — 新增日志配置项
 * ================================================================ */

// v1.1:
// host=127.0.0.1
// port=8080
// www_root=www
// user_file=data/users.csv
// log=logs/server.log
// max_connections=256
// max_request_bytes=4096
// worker_processes=2
// worker_shutdown_timeout_ms=3000

// v1.2:
// host=127.0.0.1
// port=80
// www_root=www
// user_file=data/users.csv
// log=logs/server.log
// system_log=logs/system.log        ← 新增: 系统日志路径
// access_log=logs/access.log        ← 新增: 访问日志路径
// log_max_lines=10000               ← 新增: 滚动行数阈值
// log_max_roll_files=5              ← 新增: 保留历史文件数
// max_connections=256
// max_request_bytes=4096
// worker_processes=2
// worker_shutdown_timeout_ms=3000

/* ================================================================
 * [MODIFIED] include/http_parser.h — 新增三个辅助函数声明
 * ================================================================ */

// ---- 新增头文件 ----
// #include <time.h>

// ---- 新增函数声明 ----
// 301/302 重定向响应
int http_build_redirect(int status, const char *location,
                        char *output, int size);

// 路径安全检查 (防目录穿越)
int http_is_safe_path(const char *uri);

// 获取文件修改时间 (为 304 Not Modified 预留)
time_t http_get_file_mtime(const char *filepath);

/* ================================================================
 * [MODIFIED] src/http_parser.c — 新增三个辅助函数实现
 * ================================================================ */

// ---- http_build_redirect (301/302) ----
// 构建带 Location 头的重定向响应, HTML body 包含可点击链接
// 响应格式:
//   HTTP/1.1 301 MOVED PERMANENTLY\r\n
//   Server: MiniWeb/1.2\r\n
//   Date: ...\r\n
//   Content-Type: text/html; charset=utf-8\r\n
//   Location: /blog/\r\n
//   Connection: Keep-Alive\r\n
//   \r\n
//   <!DOCTYPE html>...<a href="/blog/">/blog/</a>...

// ---- http_is_safe_path ----
// 检查 URI 是否安全, 拒绝以下模式:
//   1. 包含 ".."  (目录穿越)
//   2. 包含 '\0'  (null byte injection)
//   3. 以 '~' 开头
//   4. 包含 '\\' (反斜杠)
// 返回 1 = 安全, 0 = 不安全

// ---- http_get_file_mtime ----
// 通过 stat() 获取文件 mtime, 为未来 304 Not Modified 支持做准备

// ---- Server 头版本更新 ----
// v1.1: "Server: MiniWeb/1.1\r\n"
// v1.2: "Server: MiniWeb/1.2\r\n"

/* ================================================================
 * [MODIFIED] src/request_handler.c — Blog 路由 + 二进制文件支持
 * ================================================================ */

/*
 * 变更 1: 响应缓冲区从 16KB 扩展到 5MB
 *
 *   原因: 博客图片文件 (logo 1.3MB, banner 640KB, avatar 4.1MB)
 *   远超原有 16KB 限制。使用静态分配 (.bss 段) 避免栈溢出。
 */

// v1.1:
// #define RESP_BUF_SIZE 16384
// char body[16384];

// v1.2:
#define BLOG_BODY_MAX (5 * 1024 * 1024)
static char g_body_buf[BLOG_BODY_MAX];   // 文件读取缓冲区 (.bss)
#define RESP_BUF_SIZE (5 * 1024 * 1024 + 4096)  // 响应缓冲区 (.bss, static)

/*
 * 变更 2: 新增 GET /blog/* 路由 (在 GET / 之后, GET /hello 之前)
 *
 *   路由处理逻辑:
 *     1. 405: 非 GET/HEAD 方法 → 返回 405 Method Not Allowed
 *     2. 403: http_is_safe_path() 检测失败 → 返回 403 Forbidden
 *     3. 301: path == "/blog" (无尾斜杠) → 重定向到 /blog/
 *     4. 200: http_serve_file("blog", path+5, ...) → 返回文件内容
 *     5. 404: 文件不存在 → HTML 格式 404 页面
 */

// ---- GET / 路由: 修复返回值 ----
// v1.1:
//   if (http_build_response(...) < 0) return -1;
//   return 0;                          ← BUG: 返回 0 导致 send 长度为 0
// v1.2:
//   return http_build_response(...);   ← 修复: 返回实际响应长度

/*
 * 变更 3: 修复二进制文件发送截断
 *
 *   问题: send() 使用 strlen(resp_buf) 计算长度, 二进制图片中的
 *   0x00 字节导致 strlen() 提前截断, 只发送几个字节。
 *
 *   修复: request_handler_process_http() 改为返回实际响应长度 (>0),
 *   调用者使用该长度而非 strlen() 进行 send()。
 *
 *   影响:
 *     - build_http_response(): return 0 → return written
 *     - epoll_server.c: strlen(resp_buf) → resp_len
 *     - request_handler_handle_connection: strlen(resp_buf) → resp_len
 */

/* ================================================================
 * [MODIFIED] src/epoll_server.c — 访问日志 + 二进制发送
 * ================================================================ */

// ---- 变更 1: 响应缓冲区增大 ----
// v1.1: #define RESP_BUF_SIZE 16384
// v1.2: #define RESP_BUF_SIZE (5 * 1024 * 1024 + 4096)

// ---- 变更 2: resp_buf 从栈分配改为静态分配 ----
// v1.1: char resp_buf[RESP_BUF_SIZE];        ← 栈上 16KB
// v1.2: static char resp_buf[RESP_BUF_SIZE]; ← .bss 段 5MB

// ---- 变更 3: 新增访问日志记录 ----
// 在 send() 成功后, 关闭连接前:
//   log_access(conn->client_ip, method, path, resp_status, (int)sent);

// ---- 变更 4: send() 使用实际长度 ----
// v1.1: sent = send(ready_fd, resp_buf, strlen(resp_buf), 0);
//        → strlen 遇 0x00 截断, 二进制图片只发几个字节
// v1.2:
//   int resp_len = request_handler_process_http(&req, resp_buf, sizeof(resp_buf));
//   if (resp_len < 0) { ... 500 error ... }
//   ...
//   sent = send(ready_fd, resp_buf, (size_t)resp_len, 0);
//        → 使用构造函数返回的精确长度, 二进制文件完整发送

/* ================================================================
 * [MODIFIED] src/main.c — 日志初始化适配新 API
 * ================================================================ */

// ---- 独立模式 (epoll/fork/thread/pool/select/process) ----
// v1.1: log_init("logs/server.log")
// v1.2: log_init_single("logs/server.log")      ← 使用简化 API

// ---- Master 模式 ----
// v1.1: log_init(master_config.log_path)
// v1.2: log_init(master_config.system_log, master_config.access_log,
//                master_config.log_max_lines, master_config.log_max_roll_files)

// ---- 传统 conf 模式 ----
// v1.1: log_init(config.log_path)
// v1.2: log_init(config.system_log, config.access_log,
//                config.log_max_lines, config.log_max_roll_files)

// ---- 启动日志简化 ----
// v1.1: 三段 log_info("===..."), log_info(buf), log_info("===...")
// v1.2: 简化为 log_infof(...) 或单行 log_info(...)

/* ================================================================
 * [MODIFIED] src/master_worker.c — log_reopen 适配新 API
 * ================================================================ */

// v1.1:
//   const char *log_path = config->log_path;
//   ...
//   log_reopen(log_path);         ← 需要传入路径

// v1.2:
//   (删除 log_path 变量)
//   ...
//   log_reopen();                 ← 使用内部保存的路径, 无需传参

/* ================================================================
 * [MODIFIED] src/epoll_server_main.c — 日志初始化
 * ================================================================ */

// v1.1: log_init("logs/server.log")
// v1.2: log_init_single("logs/server.log")

/* ================================================================
 * [MODIFIED] src/request_worker.c — 日志初始化
 * ================================================================ */

// v1.1: log_init(log_path)
// v1.2: log_init_single(log_path)

/* ================================================================
 * [MODIFIED] www/index.html — 服务器首页更新
 * ================================================================ */

// ---- 版本更新 ----
// v1.1: <title>MiniWeb Server v1.1</title>
//       <div ...>Powered by epoll &middot; v1.1</div>
//       MiniWeb Server v1.1 &middot; ...
// v1.2: <title>MiniWeb Server v1.2</title>
//       <div ...>Powered by epoll &middot; v1.2</div>
//       MiniWeb Server v1.2 &middot; ...

// ---- 新增 Blog 入口 ----
// "快速体验" 区域新增橙色卡片:
//   <h3>&#128214; 实验博客</h3>
//   <p>服务器架构、调试心得、关于作者</p>
//   <a href="/blog/">进入 Blog</a>
//
// API 端点表新增行:
//   GET  /blog/  实验博客 — 架构介绍、调试心得、关于作者

/* ================================================================
 * 变更汇总
 * ================================================================
 *
 * 新增文件:
 *   blog/index.html         — 博客首页 (版本演进、特性展示、性能数据)
 *   blog/about.html         — 架构介绍 (Master-Worker、Epoll、日志系统)
 *   blog/article.html       — 实验心得 (v0.7~v1.1 完整调试记录)
 *   blog/resume.html        — 关于作者 (个人简介与项目经历)
 *   blog/css/style.css      — 成电蓝+银杏金主题 (496 行)
 *   blog/js/main.js         — 交互脚本 (进度条、鼠标跟踪、点击粒子)
 *   blog/img/logo.png       — PNG 图片
 *   blog/img/banner.jpg     — JPEG 图片
 *   blog/img/avatar.gif     — GIF 图片
 *   blog/img/photo.jpg      — 作者照片
 *   blog/icon/favicon.ico   — ICO 图标
 *   blog/README.md          — Blog 子 README
 *   tests/test_blog.sh      — Blog 测试套件 (24 项, 165 行)
 *
 * 修改文件:
 *   include/log.h            — 日志 API 重新设计 (级别/双文件/滚动)
 *   include/config.h         — 新增 4 个日志配置字段
 *   include/http_parser.h    — 新增 3 个辅助函数声明
 *   src/log.c                — 日志系统完全重写 (428 行变更)
 *   src/config.c             — 解析新增日志字段 + 默认值
 *   src/http_parser.c        — 新增 3 个函数, Server 版本更新
 *   src/request_handler.c    — Blog 路由 + 二进制发送修复 (221 行变更)
 *   src/epoll_server.c       — 访问日志 + 二进制发送修复 (90 行变更)
 *   src/main.c               — 日志初始化适配新 API
 *   src/master_worker.c      — log_reopen 参数简化
 *   src/epoll_server_main.c  — log_init → log_init_single
 *   src/request_worker.c     — log_init → log_init_single
 *   conf/server.conf         — 新增 4 个日志配置项
 *   www/index.html           — 版本号更新 + Blog 入口
 *   Makefile                 — 无需修改 (log.o 已在链接列表)
 *
 * 新增功能:
 *   1. Blog 网站: 多级目录, 7 种 MIME 类型, 完整状态码处理
 *   2. 分离式日志: 系统日志 + 访问日志, 双文件输出
 *   3. 四级日志: DEBUG/INFO/WARNING/ERROR + printf 风格 API
 *   4. 日志滚动: 行数阈值自动滚动, 保留 N 个历史文件
 *   5. HTTP 状态码: 301 (/blog→/blog/), 403 (路径穿越), 405 (方法不允许)
 *   6. 路径安全: http_is_safe_path() 防目录穿越攻击
 *   7. 鼠标跟踪: 自定义光标 + 点击粒子特效
 *
 * 修复的 Bug:
 *   1. 二进制图片发送截断: strlen(resp_buf) → resp_len (send 使用精确长度)
 *   2. GET / 响应为空: build_http_response 吞掉 http_build_response 返回值
 *   3. 栈溢出风险: 5MB 缓冲区从栈移到 .bss 段 (static)
 *   4. 缓冲区不足: 16KB → 5MB, 支持最大 5MB 静态文件
 *
 * 性能/用户体验提升:
 *   1. 鼠标跟踪: 自定义光标 + 点击粒子, 提升交互感
 *   2. 进度条: 顶部 3px 蓝金渐变, 实时反映页面滚动位置
 *   3. 卡片动画: IntersectionObserver 入场淡入
 *   4. 代码块: Mac 风格三色圆点, 深色背景
 *   5. 域名支持: miniweb.uestc, 80 端口直接访问
 * ================================================================ */
