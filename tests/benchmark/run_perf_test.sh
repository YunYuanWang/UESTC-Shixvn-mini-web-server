#!/usr/bin/env bash
# ================================================================
# run_perf_test.sh — 场景A: 短请求性能测试
#
# 使用 Python http_bench.py 进行压力测试（避免 ab/wrk2 兼容性问题）。
# 针对 4核8GB VM 优化：fork 模式使用低并发，防止系统过载。
# ================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PERF_DIR="$SCRIPT_DIR/results/perf"
BENCH="$SCRIPT_DIR/http_bench.py"

# ---- VM-SAFE config ----
DURATION=10
RUNS=2
BASE_PORT=9100

# Per-mode concurrency levels (matched to each mode's capacity)
FORK_CONN=(1 2 5)
POOL_CONN=(10 20 50)
SELECT_CONN=(10 50 100 200)

usage() {
    echo "Usage: $0 [--mode fork|pool|select|all]"
    exit 1
}
TEST_MODE="all"
[ $# -gt 0 ] && TEST_MODE="$1"

cd "$PROJECT_DIR"
make clean > /dev/null 2>&1
make > /dev/null 2>&1
echo "Build: OK"
mkdir -p "$PERF_DIR"

CSV="$PERF_DIR/latency.csv"
echo "mode,concurrency,run,qps,avg_latency_ms,max_latency_ms,p50_ms,p75_ms,p90_ms,p95_ms,p99_ms,total_requests,total_errors,duration_s" > "$CSV"

run_test() {
    local mode=$1 port=$2 conn=$3 run=$4
    rm -f logs/server.log
    ./mini_web_server "$mode" 127.0.0.1 "$port" > /dev/null 2>&1 &
    local spid=$!
    sleep 0.5

    # Parse benchmark output
    local output
    output=$(python3 "$BENCH" --url "http://127.0.0.1:$port/hello" -c "$conn" -d "$DURATION" 2>&1) || true

    # Extract metrics
    local qps avg_lat max_lat p50 p75 p90 p95 p99 total errs
    qps=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')
    avg_lat=$(echo "$output" | grep "Avg latency:" | awk '{print $3}')
    max_lat=$(echo "$output" | grep "Max latency:" | awk '{print $3}')
    p50=$(echo "$output" | grep "P50 latency:" | awk '{print $3}')
    p75=$(echo "$output" | grep "P75 latency:" | awk '{print $3}')
    p90=$(echo "$output" | grep "P90 latency:" | awk '{print $3}')
    p95=$(echo "$output" | grep "P95 latency:" | awk '{print $3}')
    p99=$(echo "$output" | grep "P99 latency:" | awk '{print $3}')
    total=$(echo "$output" | grep "Total requests:" | awk '{print $3}')
    errs=$(echo "$output" | grep "Total errors:" | awk '{print $3}')

    qps=${qps:-0}; avg_lat=${avg_lat:-0}; max_lat=${max_lat:-0}
    p50=${p50:-0}; p75=${p75:-0}; p90=${p90:-0}; p95=${p95:-0}; p99=${p99:-0}
    total=${total:-0}; errs=${errs:-0}

    echo "$mode,$conn,$run,$qps,$avg_lat,$max_lat,$p50,$p75,$p90,$p95,$p99,$total,$errs,$DURATION" >> "$CSV"

    kill -9 $spid 2>/dev/null || true
    wait $spid 2>/dev/null || true
    sleep 2
    echo "  $mode c=$conn r=$run: QPS=$qps errs=$errs avg=${avg_lat}ms"
}

echo "=============================================="
echo "  Performance Benchmark (VM-Safe)"
echo "=============================================="
echo ""

for MODE in fork pool select; do
    [ "$TEST_MODE" != "all" ] && [ "$TEST_MODE" != "$MODE" ] && continue

    if [ "$MODE" = "fork" ]; then CONN_LEVELS=("${FORK_CONN[@]}"); fi
    if [ "$MODE" = "pool" ]; then CONN_LEVELS=("${POOL_CONN[@]}"); fi
    if [ "$MODE" = "select" ]; then CONN_LEVELS=("${SELECT_CONN[@]}"); fi

    for CONN in "${CONN_LEVELS[@]}"; do
        PORT=$((BASE_PORT + RANDOM % 5000))
        for RUN in $(seq 1 $RUNS); do
            run_test "$MODE" "$PORT" "$CONN" "$RUN"
        done
    done
    echo "--- $MODE complete ---"
done

echo ""
echo "Results: $CSV"
