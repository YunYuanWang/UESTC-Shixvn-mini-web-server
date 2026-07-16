# mini_web_server 并发模型对比测试

## 概述

本测试套件对 mini_web_server 的三种并发模型进行系统对比：

| 版本 | 模式 | 并发模型 | 启动命令 |
|------|------|----------|----------|
| v0.7 | fork | 多进程（每连接 fork 一个子进程） | `./mini_web_server fork [ip] [port]` |
| v0.8 | pool | 线程池（2→8 动态扩缩） | `./mini_web_server pool [ip] [port]` |
| v0.9 | select | 单线程 I/O 多路复用 | `./mini_web_server select <ip> <port>` |

## 目录结构

```
tests/benchmark/
├── README.md               # 本文档
├── common.sh               # 公共函数库
├── run_perf_test.sh        # 场景A：短请求性能测试
├── run_long_conn_test.sh   # 场景B：大数据集长处理时间测试
├── parse_wrk_output.py     # wrk2 输出解析
├── plot_results.py         # 数据可视化
└── results/
    ├── perf/               # 场景A 原始数据 + CSV
    ├── long_conn/          # 场景B 原始数据 + CSV
    └── charts/             # 生成的图表（PNG, 150 DPI）
```

## 前置条件

| 工具 | 用途 | 安装方式 |
|------|------|----------|
| ab (ApacheBench) | HTTP 压力测试 | 已安装 (`/usr/bin/ab`) |
| wrk2 | HTTP/1.1 恒定速率压力测试（支持 keep-alive） | 需编译安装 |
| pidstat | 进程 CPU/内存/上下文切换监控 | `apt install sysstat` |
| ss | 连接状态统计 | 已内置 |
| Python 3 + matplotlib | 数据可视化 | `conda activate ServerTest` |

> **v0.10 更新**：服务器已实现 HTTP/1.1 keep-alive 支持（三种模式均支持连接复用，空闲 5s 超时，单连接最多 100 个请求），wrk2 现已可正常使用。ab 仍然可用作短连接场景对比。

## 测试方案设计

### 场景A：短请求性能测试

**目的**：测试各模型在极短请求下的极限吞吐量和响应延迟。

**端点**：`GET /hello`（立即返回 `Hello, Web!\n`，处理时间 < 0.1ms）

**测试矩阵**（VM 安全版，4核8GB 环境）：

| 并发连接数 | 测试时长 | 轮次 | 说明 |
|------------|----------|------|------|
| 10 | 15s | ×3 | 低负载基准 |
| 50 | 15s | ×3 | 中等负载 |
| 100 | 15s | ×3 | 高负载 |
| 200 | 15s | ×3 | 极限（fork 模式自动跳过 >100） |

**高配机器**：使用 `--aggressive` 参数可测试 100/500 级别。

**Fork 模式限制**：最大 100 连接（避免 4 核机器上创建过多进程）

**测试流程**（每种模式 × 每级并发）：
1. 启动服务器
2. 预热（`curl` 一次 /hello）
3. 启动 `pidstat` 后台采集（每秒：CPU%、RSS、上下文切换）
4. 运行 `ab -c $CONN -t $DURATION -q`
5. 停止 pidstat，停止服务器
6. 解析 ab 输出，写入 CSV

**ab 参数说明**：
- `-c`：并发连接数
- `-t`：测试持续秒数（ab 在该时间内尽可能多地发送请求）
- `-q`：静默模式（抑制进度输出，便于日志解析）

**fork 模式特别处理**：
- 检查 `ulimit -u`（最大用户进程数），连接数接近上限时跳过
- 1000+ 并发默认跳过（可用 `--skip-fork-high` 控制）
- fork 3000 会创建 3000 个子进程，预期会触发进程数/内存瓶颈——这是正常的对比结论

**采集指标**：

| 指标 | 来源 | 说明 |
|------|------|------|
| QPS | wrk2/ab `Requests/sec` | 实际吞吐量 |
| Avg Latency | wrk2/ab `Thread Stats` / `Time per request` | 平均响应时间 |
| Max Latency | wrk2/ab 延迟分布 | 最大响应时间 |
| P50/P75/P90/P95/P99/P99.9 | wrk2 `Latency Distribution` | 延迟分位数 |
| Socket Errors | wrk2 `Socket errors` | 连接/读写/超时错误 |
| CPU% | pidstat `-u` | 进程 CPU 占用 |
| RSS (KB) | pidstat `-r` | 物理内存占用 |
| cswch/s | pidstat `-w` | 每秒自愿上下文切换 |

**VM 安全机制**（4核8GB 环境）：

| 机制 | 说明 |
|------|------|
| CPU 空闲检查 | 每次测试前检查 CPU 空闲率，<20% 自动冷却 10s，仍不足则跳过 |
| 工具速率限制 | 使用合理的目标速率，非 1M |
| 工具线程数 | 固定 2 线程，剩余 2 核留给服务器 |
| nice 调度 | 压测工具以 `nice -n 10` 运行，服务器有更高优先级 |
| timeout 保护 | 每次压测有 `timeout` 超时，防止挂死 |
| 崩溃检测 | 测试后检查服务器进程存活，崩溃记录 -1 |
| 冷却间隔 | 每次运行后 5s 冷却，模式切换后 15s |
| Fork 上限 | 默认最大 100 连接（`FORK_MAX_CONN=100`），高并发模式上限 200 |

### 场景B：大数据集长处理时间测试

**目的**：用真实数据集模拟"长连接"（长处理时间）场景。不使用 `/sleep` 人为延迟，而是用 CPU 密集型数据操作真实反映各模型对计算资源的调度能力。

**数据集**：`data/users_100000.csv`（100K 用户，3.7MB，约 100K 条记录）

**端点**：`GET /users/compare/<name>`

该端点执行 **10,000 次连续搜索**——链表遍历（O(n)）+ BST 查找（O(log n)）各 10,000 次。在 100K 用户数据集下，总遍历量约 5 亿次节点比较，单次请求 5~30 秒。

**关键特性**：
- **CPU 密集型**：纯内存计算，无 I/O 等待
- **响应体小**：几百字节的对比报告，网络不构成瓶颈
- **链表遍历 cache miss**：节点不连续存储，真实反映内存访问模式

**端点对比**：

| 端点 | 处理方式 | 瓶颈 | 适用场景 |
|------|----------|------|----------|
| `/sleep/500` | 线程睡眠 | 无（不消耗 CPU） | 模拟 I/O 等待 |
| `/users/compare/<name>` | CPU 计算 | CPU / 内存带宽 | 模拟真实业务计算 |

**测试矩阵**：

| 并发连接数 | 测试时长 | 请求超时 | 说明 |
|------------|----------|-----------|------|
| 5 | 120s | 60s | 低并发基准 |
| 10 | 120s | 60s | 中等并发 |
| 20 | 120s | 60s | 高并发慢请求压力 |
| 50 | 120s | 60s | 极限并发 |

**关键观察点**：

| 模式 | 预期行为 |
|------|----------|
| **Select** | 单线程事件循环——一个 `/users/compare` 计算时**阻塞所有其他连接**。响应时间接近 N × 单请求时间的串行化 |
| **Pool** | 线程池最多 8 个 worker——最多同时处理 8 个慢请求，其余在队列排队。队列积压时 latency 线性增长 |
| **Fork** | 每个请求独立进程——真正的并行，但进程调度开销大，内存随进程数线性增长 |

**数据准备**：
```bash
# 测试前
cp data/users.csv data/users.csv.bak
cp data/users_100000.csv data/users.csv

# 测试后恢复
cp data/users.csv.bak data/users.csv
```
脚本已内置自动切换逻辑。

**采集指标**（每秒采样，用于时间序列图）：

| 指标 | 来源 | 说明 |
|------|------|------|
| CPU% | `ps -p <PID> -o %cpu` | 每秒 CPU 利用率 |
| RSS (KB) | `ps -p <PID> -o rss` | 物理内存占用 |
| 进程数 | `pgrep -P <PID> | wc -l` | fork 模式子进程数 |
| 线程数 | `ps -p <PID> -o nlwp` | pool 模式线程数 |
| 上下文切换 | `/proc/<PID>/status` | 自愿上下文切换累计值 |
| 连接数 | `ss -tnp | grep :<PORT> | wc -l` | 当前 TCP 连接数 |

## 运行测试

### 1. 编译

```bash
cd /home/zigh-wang/data-disk/shixvn/miniwebserver
make clean && make
```

### 2. 运行性能测试（约 30-45 分钟）

```bash
# 全部三种模式
bash tests/benchmark/run_perf_test.sh

# 仅测试单个模式
bash tests/benchmark/run_perf_test.sh --mode pool

# 自定义并发级别
bash tests/benchmark/run_perf_test.sh --conn 100,200,500,1000

# 跳过 fork 高并发
bash tests/benchmark/run_perf_test.sh --skip-fork-high
```

### 3. 运行长连接测试（约 15-25 分钟）

```bash
# 全部三种模式
bash tests/benchmark/run_long_conn_test.sh

# 仅测试 select 模式
bash tests/benchmark/run_long_conn_test.sh --mode select

# 自定义并发和时长
bash tests/benchmark/run_long_conn_test.sh --conn 5,10,20 --duration 180
```

### 4. 生成图表

```bash
conda run -n ServerTest python3 tests/benchmark/plot_results.py
```

图表输出到 `tests/benchmark/results/charts/`。

## 图表清单

### 场景A：性能测试图表

| # | 文件名 | 类型 | 内容 |
|---|--------|------|------|
| 1 | `01_avg_latency.png` | 三线折线图 | 并发连接数 vs 平均响应时间 |
| 2 | `02_p95_latency.png` | 三线折线图 | 并发连接数 vs P95 响应时间 |
| 3 | `03_qps_throughput.png` | 三线折线图 | 并发连接数 vs QPS 吞吐量 |
| 4 | `04_cpu_usage.png` | 分组柱状图 | 并发连接数 vs CPU 利用率 |
| 5 | `05_memory_usage.png` | 分组柱状图 | 并发连接数 vs 内存 RSS |
| 6 | `06_perf_dashboard.png` | 2×2 仪表盘 | 延迟 + QPS + P95 + CPU 汇总 |

### 场景B：稳定性和长连接图表

| # | 文件名 | 类型 | 内容 |
|---|--------|------|------|
| 7 | `07_stability_cpu.png` | 时间序列折线图 | CPU 利用率随时间变化（三模式叠加） |
| 8 | `08_stability_memory.png` | 时间序列折线图 | 内存 RSS 随时间变化 |
| 9 | `09_stability_cswch.png` | 时间序列折线图 | 上下文切换速率随时间变化 |
| 10 | `10_stability_proc_count.png` | 时间序列折线图 | 进程/线程数随时间变化 |
| 11 | `11_stability_dashboard.png` | 2×2 仪表盘 | CPU + 内存 + CS + 进程数汇总 |
| 12 | `12_radar_comparison.png` | 雷达图 | 六维度综合对比（1000 连接级别） |

## 预期分析结论

### 短请求场景 (`/hello`)

| 指标 | Fork | Pool | Select |
|------|------|------|--------|
| 低并发 QPS | ★★☆ | ★★★ | ★★★ |
| 高并发 QPS | ★☆☆（衰减明显） | ★★☆ | ★★★ |
| 低并发延迟 | ★★☆ | ★★★ | ★★★ |
| P95 尾延迟 | ★★☆ | ★★☆ | ★★★（最低） |
| CPU 效率 | ★☆☆（进程调度开销大） | ★★☆ | ★★★ |
| 内存效率 | ★☆☆（每进程复制页表） | ★★☆ | ★★★ |

**原因分析**：
- **Select**：无锁、无上下文切换开销，单线程事件循环在短请求场景下是理论最优方案。瓶颈是 `select()` 本身 O(n) 的 fd 扫描，在 1024+ 连接时开始显现
- **Pool**：线程池复用降低创建销毁开销，但线程间 `pthread_mutex_t` 竞争和上下文切换仍是瓶颈
- **Fork**：进程创建 (`fork()`) + 销毁 + `waitpid()` 开销极大，3000 个进程的调度压力使系统表现急剧恶化

### 长处理场景 (`/users/compare/<name>`)

| 指标 | Fork | Pool | Select |
|------|------|------|--------|
| 请求并行度 | ★★★（真正并行） | ★★☆（最多 8 路并行） | ★☆☆（完全串行） |
| 响应时间（低并发） | ★★☆ | ★★★ | ★☆☆（阻塞最严重） |
| CPU 利用率稳定性 | ★★☆ | ★★★ | ★☆☆（单核瓶颈） |
| 内存增长 | ★☆☆（每进程增长） | ★★☆ | ★★★（几乎不变） |
| 上下文切换 | ★☆☆（3000 进程调度） | ★★☆（≤8 线程） | ★★★（几乎为零） |

**原因分析**：
- **Select**：`/users/compare` 是纯 CPU 计算——单线程下，一个请求计算 10 秒意味着**所有其他 49 个连接都要等 10 秒**。这是 select 模式的阿克琉斯之踵，也是为什么现代服务器使用 epoll + 非阻塞 I/O + worker 线程池的混合架构
- **Pool**：8 个 worker 线程可以同时处理 8 个慢请求，其余在队列排队。这是 CPU 密集型场景下的合理平衡
- **Fork**：操作系统将 50 个 `/users/compare` 请求调度到 50 个进程，在多核 CPU 上真正并行计算。但代价是 50 个进程的地址空间（每个含 100K 用户数据），内存膨胀显著

### 选型建议

| 场景 | 推荐 | 原因 |
|------|------|------|
| 静态文件 / API 网关（大量短连接） | **Select** | 最高吞吐，最低延迟 |
| 计算密集型后端服务 | **Pool** | 线程池并行 + 可控资源 |
| 需要进程隔离的安全敏感服务 | **Fork** | 独立地址空间，一个进程崩溃不影响其他 |
| 混合型（短请求 + 偶发长请求） | **Pool + epoll** | 短请求快速响应 + 长请求不阻塞（需升级为 epoll） |

## 故障排查

### 端口被占用
```bash
fuser -k <PORT>/tcp
# 或检查残留进程：
pkill -f mini_web_server
```

### fork 模式无法启动（资源不足）
```bash
ulimit -u   # 查看最大进程数
# 临时提高：
ulimit -u 65535
```

### 压测工具报连接超时
- 检查 `net.ipv4.ip_local_port_range`：`cat /proc/sys/net/ipv4/ip_local_port_range`
- TIME_WAIT 过多时可调整：`sysctl net.ipv4.tcp_tw_reuse=1`

### select 模式连接被拒绝
- `SELECT_MAX_CONNS=1024` 是硬限制，超过即拒绝。对于 3000 并发测试，预期部分连接被拒绝——这本身就是测试结论的一部分

## 参考资料

- wrk2: https://github.com/giltene/wrk2 （需服务器 HTTP/1.1 keep-alive 支持，v0.10+ 已支持）
- ab: `man ab`
- pidstat: `man pidstat`
- 服务端点：`curl http://HOST:PORT/help` 查看完整 API 列表
