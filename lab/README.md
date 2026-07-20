# Lab — Mini Web Server 技术实验室

这是挂载在 Mini Web Server 上的第二个实验站点，用于验证多网站虚拟主机（server block）功能。

## 与 Blog 站点的差异

| 特性 | Blog（博客） | Lab（实验室） |
|------|-------------|--------------|
| 配色 | 成电蓝 + 银杏金 | 深青 + 翠绿 |
| 定位 | 项目展示 + 开发日志 | 技术实验 + 学习文档 |
| 图标 | logo.png | CSS emoji 🧪 |
| 背景特效 | 蓝色径向渐变 | 青色径向渐变 |
| 粒子颜色 | 蓝/金色系 | 青/绿色系 |

## 目录结构

```
lab/
├── index.html          # 实验室首页 — 实验模块、技术指标、快速开始
├── css/
│   └── style.css       # 深青+翠绿主题样式表
├── js/
│   └── main.js         # 前端交互脚本（青色主题）
├── icon/
│   └── favicon.ico     # 网站图标
└── README.md           # 本文件
```

## 多网站测试

启动服务器后，可通过不同 Host 头访问两个站点：

```bash
# Blog 站点（蓝/金色）
curl -H "Host: blog.local" http://127.0.0.1:80/

# Lab 站点（青/绿色）
curl -H "Host: lab.local" http://127.0.0.1:80/

# 浏览器测试
# 1. 在 /etc/hosts 中添加:
#    127.0.0.1 blog.local lab.local
# 2. 浏览器访问:
#    http://blog.local/lab/  → Lab 站点
#    http://lab.local/       → Lab 站点
```
