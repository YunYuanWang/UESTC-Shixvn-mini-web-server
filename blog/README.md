# Blog — Mini Web Server 实验博客

这是挂载在 Mini Web Server v1.2 上的实验博客网站，用于验证服务器的静态文件服务能力。

## 目录结构

```
blog/
├── index.html          # 博客首页 — 特性展示、性能基准数据
├── about.html          # 架构介绍 — Epoll + Master-Worker 详解
├── article.html        # 实验心得 — Keep-Alive 三轮排错经历
├── css/
│   └── style.css       # 响应式样式表
├── js/
│   └── main.js         # 前端交互脚本
├── img/
│   ├── logo.png        # PNG 格式 → MIME: image/png
│   ├── banner.jpg      # JPEG 格式 → MIME: image/jpeg
│   └── avatar.gif      # GIF 格式 → MIME: image/gif
├── icon/
│   └── favicon.ico     # ICO 格式 → MIME: image/x-icon
└── README.md           # 本文件
```

## 页面说明

### index.html — 首页
- 服务器核心特性卡片（Epoll、Master-Worker、Keep-Alive、多模式）
- 性能基准测试对比表（Keep-Alive ON: 2,543 req/s vs OFF: 764 req/s）
- 快速导航链接

### about.html — 架构介绍
- Master-Worker 进程模型 ASCII 架构图
- Epoll 事件驱动原理（epoll_create1 → epoll_ctl → epoll_wait）
- Keep-Alive 连接管理（超时清理、优雅关闭）
- 六种并发模式对比表

### article.html — 实验心得
记录了 v1.1 开发中 Keep-Alive 功能的三轮排错过程：

| 轮次 | 问题 | 根因 | 修复 |
|------|------|------|------|
| 第一轮 | ab 测试 Keep-Alive 请求数为 0 | Connection 头大小写不匹配 | 增加 4 种大小写组合检测 |
| 第二轮 | 响应头 Connection 不一致 | 全小写 vs 首字母大写 | 统一使用 `Keep-Alive` |
| 第三轮 | 高负载下 Connection reset | 直接 close() 导致 TCP RST | safe_close: shutdown + drain + close |
| 第四轮 | /users 端点挂起 | pipe 缓冲区死锁 | 改为直接内存 buffer 写入 |

## 验证的 HTTP 状态码

| 状态码 | 触发条件 | 测试命令 |
|--------|---------|---------|
| 200 | 正常访问 | `curl http://127.0.0.1:8080/blog/` |
| 301 | `/blog` 无尾斜杠 | `curl -I http://127.0.0.1:8080/blog` |
| 403 | 路径穿越 | `curl --path-as-is http://127.0.0.1:8080/blog/../etc/passwd` |
| 404 | 文件不存在 | `curl http://127.0.0.1:8080/blog/nonexist` |
| 405 | 方法不允许 | `curl -X POST http://127.0.0.1:8080/blog/` |

## 验证的 MIME 类型

| 文件 | 预期 Content-Type |
|------|------------------|
| `index.html` | `text/html; charset=utf-8` |
| `css/style.css` | `text/css; charset=utf-8` |
| `js/main.js` | `application/javascript` |
| `img/logo.png` | `image/png` |
| `img/banner.jpg` | `image/jpeg` |
| `img/avatar.gif` | `image/gif` |
| `icon/favicon.ico` | `image/x-icon` |

## 测试

```bash
# 启动服务器
./mini_web_server epoll 127.0.0.1 8080

# 运行 Blog 测试套件
bash tests/test_blog.sh

# 浏览器访问
open http://127.0.0.1:8080/blog/
```
