#!/usr/bin/env bash
# ================================================================
# run_long_conn_test.sh — 场景B: 大数据集长处理时间测试
#
# 使用 100K 用户数据集，/users/compare/<name> CPU密集型端点。
# 通过 Python http_bench.py 进行压力测试，60s 超时适应长请求。
# ================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
LONG_DIR="$SCRIPT_DIR/results/long_conn"
BENCH="$SCRIPT_DIR/http_bench.py"

# ---- VM-SAFE config ----
CONCURRENCY_LEVELS=(5 10)
FORK_MAX_CONN=20
DURATION=90
BASE_PORT=9500
ENDPOINT="/users/compare/ZhangSan"
LARGE_CSV="data/users_100000.csv"
ORIGINAL_CSV="data/users.csv"
REQ_TIMEOUT=60  # /users/compare takes 5-30s per request

# ---- helpers ----
log_info()  { echo -e "\033[0;32m[INFO]\033[0m  $*"; }
log_warn()  { echo -e "\033[1;33m[WARN]\033[0m  $*"; }
log_error() { echo -e "\033[0;31m[ERROR]\033[0m $*"; }
log_step()  { echo -e "\033[0;34m[STEP]\033[0m  $*"; }

usage() {
    echo "Usage: $0 [--mode pool|select|all]"
    exit 1
}
TEST_MODE="all"
[ $# -gt 0 ] && TEST_MODE="$1"

MODES=()
if [ "$TEST_MODE" = "all" ]; then
    MODES=(pool select)
else
    MODES=("$TEST_MODE")
fi

echo ""
echo "=============================================="
echo "  Long Connection Stability Test"
echo "=============================================="
echo "  Dataset:    $LARGE_CSV (100K users)"
echo "  Endpoint:   $ENDPOINT (CPU-intensive)"
echo "  Timeout:    ${REQ_TIMEOUT}s per request"
echo "  Concurrency: ${CONCURRENCY_LEVELS[*]}"
echo "  Modes:      ${MODES[*]}"
echo "  Duration:   ${DURATION}s per test"
echo "=============================================="
echo ""

# verify dataset
if [ ! -f "$PROJECT_DIR/$LARGE_CSV" ]; then
    log_error "Dataset not found: $PROJECT_DIR/$LARGE_CSV"
    exit 1
fi

# build
cd "$PROJECT_DIR"
make clean > /dev/null 2>&1
make > /dev/null 2>&1
log_info "Build: OK"

# backup + switch dataset
cp "$ORIGINAL_CSV" "${ORIGINAL_CSV}.long_bak"
cp "$LARGE_CSV" "$ORIGINAL_CSV"
log_info "Switched to large dataset"

mkdir -p "$LONG_DIR"
CSV="$LONG_DIR/stability.csv"
echo "timestamp,mode,concurrency,cpu_pct,rss_kb,vsz_kb,cswch_per_sec,nvcswch_per_sec,process_count,thread_count" > "$CSV"

# cleanup handler — restore original CSV on exit
cleanup() {
    log_info "Restoring original dataset..."
    if [ -f "${ORIGINAL_CSV}.long_bak" ]; then
        mv "${ORIGINAL_CSV}.long_bak" "$ORIGINAL_CSV"
    fi
    pkill -9 -f "mini_web_server" 2>/dev/null || true
}
trap cleanup EXIT

# ---- metrics collector (background) ----
collect_metrics() {
    local mode=$1 output=$2 port=$3 spid=$4
    echo "timestamp,mode,concurrency,cpu_pct,rss_kb,vsz_kb,cswch_per_sec,nvcswch_per_sec,process_count,thread_count" > "$output"
    local t0
    t0=$(date +%s)
    while true; do
        local now elapsed cpu rss th cswch pcount
        now=$(date +%s)
        elapsed=$((now - t0))
        [ "$elapsed" -ge "$DURATION" ] && break

        cpu=0; rss=0; th=0; pcount=1; cswch=0
        if [ -n "$spid" ] && kill -0 "$spid" 2>/dev/null; then
            read -r cpu rss th <<< $(ps -p "$spid" -o %cpu=,rss=,nlwp= 2>/dev/null || echo "0 0 0")
            cpu=${cpu:-0}; rss=${rss:-0}; th=${th:-0}
            cswch=$(grep "voluntary_ctxt_switches:" /proc/"$spid"/status 2>/dev/null | awk '{print $2}' || echo "0")
            if [ "$mode" = "fork" ]; then
                pcount=$(pgrep -P "$spid" 2>/dev/null | wc -l)
                pcount=$((pcount + 1))
            fi
        else
            break
        fi
        echo "$elapsed,$mode,0,$cpu,$rss,0,$cswch,0,$pcount,$th" >> "$output"
        sleep 1
    done
}

# ---- port management ----
free_port() {
    local port=$1
    fuser -k ${port}/tcp 2>/dev/null || true
    sleep 0.2
}
wait_port() {
    local port=$1 i
    for i in $(seq 1 20); do
        ss -tlnp 2>/dev/null | grep -q ":${port} " && return 0
        sleep 0.2
    done
    return 1
}

# ---- main test loop ----
for MODE in "${MODES[@]}"; do
    for CONN in "${CONCURRENCY_LEVELS[@]}"; do
        PORT=$((BASE_PORT + RANDOM % 5000))
        free_port "$PORT"

        echo ""
        log_step "=== Mode=$MODE  Conn=$CONN  Duration=${DURATION}s ==="

        # start server
        rm -f logs/server.log
        cd "$PROJECT_DIR"
        ./mini_web_server "$MODE" 127.0.0.1 "$PORT" > /dev/null 2>&1 &
        SPID=$!

        if ! wait_port "$PORT"; then
            log_error "Server failed to start"
            kill -9 $SPID 2>/dev/null || true
            continue
        fi
        log_info "Server PID=$SPID on port $PORT"

        # verify endpoint
        log_info "Verifying endpoint (may take 30s+)..."
        timeout 90 curl -s "http://127.0.0.1:${PORT}${ENDPOINT}" > /tmp/long_verify.txt 2>&1 || true
        if grep -q "Search Method Comparison" /tmp/long_verify.txt 2>/dev/null; then
            log_info "Endpoint OK"
        else
            log_warn "Endpoint verify issue: $(head -3 /tmp/long_verify.txt 2>/dev/null)"
        fi
        rm -f /tmp/long_verify.txt

        # start metrics collector (background)
        METRICS_FILE="$LONG_DIR/metrics_${MODE}_c${CONN}.csv"
        collect_metrics "$MODE" "$METRICS_FILE" "$PORT" "$SPID" &
        METRICS_PID=$!

        # run Python benchmark with long timeout
        log_info "Benchmarking (${DURATION}s, timeout=${REQ_TIMEOUT}s)..."
        BENCH_OUT="$LONG_DIR/bench_${MODE}_c${CONN}.txt"
        python3 "$BENCH" --url "http://127.0.0.1:${PORT}${ENDPOINT}" \
            -c "$CONN" -d "$DURATION" --timeout "$REQ_TIMEOUT" > "$BENCH_OUT" 2>&1 || true

        # stop metrics
        kill $METRICS_PID 2>/dev/null || true
        wait $METRICS_PID 2>/dev/null || true

        # parse results
        if grep -q "Requests/sec" "$BENCH_OUT" 2>/dev/null; then
            QPS=$(grep "Requests/sec:" "$BENCH_OUT" | awk '{print $2}')
            TOTAL=$(grep "Total requests:" "$BENCH_OUT" | awk '{print $3}')
            ERRS=$(grep "Total errors:" "$BENCH_OUT" | awk '{print $3}')
            AVG=$(grep "Avg latency:" "$BENCH_OUT" | awk '{print $3}')
            log_info "Result: QPS=$QPS  Total=$TOTAL  Errs=$ERRS  AvgLat=${AVG}ms"
        else
            log_warn "Benchmark may have failed (check $BENCH_OUT)"
        fi

        # stop server
        kill -INT $SPID 2>/dev/null || true
        sleep 0.5
        kill -9 $SPID 2>/dev/null || true
        wait $SPID 2>/dev/null || true
        free_port "$PORT"
        sleep 3
    done
    log_info "=== Mode $MODE complete ==="
done

echo ""
echo "=============================================="
log_info "Long Connection Stability Test complete!"
log_info "Results: $LONG_DIR/"
echo "=============================================="
