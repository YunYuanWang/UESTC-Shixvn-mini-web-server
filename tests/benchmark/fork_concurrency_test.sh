#!/usr/bin/env bash
# ================================================================
# fork_concurrency_test.sh — Fork 模式真实并发能力测试
#
# 测试方式：N 个线程同时各发 1 个请求（模拟 curl 并发），
#           不是持续轰炸。测量 fork 能同时处理多少连接。
# ================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULT_DIR="$SCRIPT_DIR/results/fork_concurrency"
PORT=9995

mkdir -p "$RESULT_DIR"

cd "$PROJECT_DIR"
make clean > /dev/null 2>&1
make > /dev/null 2>&1

echo "=============================================="
echo "  Fork 并发能力测试（一次性并发，非持续吞吐）"
echo "=============================================="
echo ""

# 测试的并发级别：逐步增大找到上限
CONCURRENCY_LEVELS=(1 5 10 20 50 100 200 300 500 700 1000)

for CONN in "${CONCURRENCY_LEVELS[@]}"; do
    # 启动全新的 fork 服务器
    pkill -9 -f "mini_web_server" 2>/dev/null || true
    sleep 0.3
    fuser -k ${PORT}/tcp 2>/dev/null || true
    sleep 0.2
    rm -f logs/server.log

    ./mini_web_server fork 127.0.0.1 "$PORT" > /dev/null 2>&1 &
    SPID=$!
    sleep 0.5

    if ! kill -0 "$SPID" 2>/dev/null; then
        echo "FATAL: fork server failed to start"; exit 1
    fi

    # 运行一次性并发测试
    RESULT=$(python3 -c "
import socket, time, threading, sys

N = $CONN
results = []
lock = threading.Lock()
barrier = threading.Barrier(N)  # 所有线程同时出发

def one_shot(i):
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(('127.0.0.1', $PORT))
        barrier.wait()  # 等所有线程都连上再一起发
        t0 = time.time()
        s.sendall(b'GET /hello HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n')
        data = s.recv(4096)
        t1 = time.time()
        ok = b'200 OK' in data
        with lock: results.append((i, ok, len(data), (t1-t0)*1000))
    except Exception as e:
        with lock: results.append((i, False, 0, str(e)))
    finally:
        if s:
            try: s.close()
            except: pass

threads = [threading.Thread(target=one_shot, args=(i,)) for i in range(N)]
for t in threads: t.start()
for t in threads: t.join(timeout=30)

success = [r for r in results if r[1]]
failed  = [r for r in results if not r[1]]
lats    = [r[3] for r in success if isinstance(r[3], (int,float))]

print(f'SUCCESS:{len(success)}', flush=True)
print(f'FAILED:{len(failed)}', flush=True)
if lats:
    lats.sort()
    n = len(lats)
    avg = sum(lats)/n
    print(f'LAT_MIN:{lats[0]:.2f}', flush=True)
    print(f'LAT_AVG:{avg:.2f}', flush=True)
    print(f'LAT_MAX:{lats[-1]:.2f}', flush=True)
    print(f'LAT_P50:{lats[n//2]:.2f}', flush=True)
    print(f'LAT_P95:{lats[n*95//100]:.2f}', flush=True)
    print(f'LAT_P99:{lats[n*99//100]:.2f}', flush=True)
if failed:
    err_types = {}
    for f in failed:
        e = str(f[3])
        err_types[e] = err_types.get(e, 0) + 1
    for e, cnt in err_types.items():
        print(f'ERR:{e}:{cnt}', flush=True)
" 2>&1)

    # 检查服务器是否还活着
    SERVER_ALIVE="alive"
    if ! kill -0 "$SPID" 2>/dev/null; then
        SERVER_ALIVE="crashed"
    fi

    # 检查僵尸进程
    ZOMBIES=$(ps aux | grep defunct | grep -v grep | wc -l)

    # 解析结果
    SUCCESS=$(echo "$RESULT" | grep "^SUCCESS:" | cut -d: -f2)
    FAILED=$(echo "$RESULT" | grep "^FAILED:" | cut -d: -f2)
    AVG_LAT=$(echo "$RESULT" | grep "^LAT_AVG:" | cut -d: -f2)
    P95_LAT=$(echo "$RESULT" | grep "^LAT_P95:" | cut -d: -f2)
    MAX_LAT=$(echo "$RESULT" | grep "^LAT_MAX:" | cut -d: -f2)
    MIN_LAT=$(echo "$RESULT" | grep "^LAT_MIN:" | cut -d: -f2)

    SUCCESS=${SUCCESS:-0}
    FAILED=${FAILED:-0}
    AVG_LAT=${AVG_LAT:-0}
    P95_LAT=${P95_LAT:-0}

    # 输出一行结果
    if [ "$FAILED" -eq 0 ]; then
        printf "  c=%-4s  ✅ %3s/%-3s 成功  | 延迟: min=%-7s avg=%-7s P95=%-7s max=%-7s | 服务器: %s 僵尸: %s\n" \
            "$CONN" "$SUCCESS" "$CONN" "${MIN_LAT}ms" "${AVG_LAT}ms" "${P95_LAT}ms" "${MAX_LAT}ms" "$SERVER_ALIVE" "$ZOMBIES"
        echo "$CONN,$SUCCESS,$FAILED,$MIN_LAT,$AVG_LAT,$P95_LAT,$MAX_LAT,$SERVER_ALIVE,$ZOMBIES" >> "$RESULT_DIR/fork_concurrency.csv"
    else
        echo "  c=%-4s  ⚠️  %3s/%-3s 成功  %s失败 | 服务器: %s 僵尸: %s" "$CONN" "$SUCCESS" "$CONN" "$FAILED" "$SERVER_ALIVE" "$ZOMBIES"
        echo "$CONN,$SUCCESS,$FAILED,0,0,0,0,$SERVER_ALIVE,$ZOMBIES" >> "$RESULT_DIR/fork_concurrency.csv"
        # 打印错误详情
        echo "$RESULT" | grep "^ERR:" | while read line; do
            echo "         └─ $(echo $line | cut -d: -f2-3)"
        done
    fi

    # 如果失败率 > 50% 或服务器崩溃，停止测试
    if [ "$FAILED" -gt $((CONN / 2)) ] || [ "$SERVER_ALIVE" = "crashed" ]; then
        echo ""
        echo "⚠️  失败率超过 50% 或服务器崩溃，达到并发上限。停止测试。"
        break
    fi

    # 冷却
    sleep 2
done

# 清理
pkill -9 -f "mini_web_server" 2>/dev/null || true

echo ""
echo "=============================================="
echo "  结果保存至: $RESULT_DIR/fork_concurrency.csv"
echo "=============================================="
