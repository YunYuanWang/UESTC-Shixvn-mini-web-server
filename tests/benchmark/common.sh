#!/usr/bin/env bash
# ================================================================
# common.sh — shared helper functions for benchmark scripts
# ================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "${BLUE}[STEP]${NC}  $*"; }

# ---- OS limits check ----
check_os_limits() {
    local max_procs max_fds port_range
    max_procs=$(ulimit -u 2>/dev/null || echo "unknown")
    max_fds=$(ulimit -n 2>/dev/null || echo "unknown")
    port_range=$(cat /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null || echo "unknown")

    log_info "OS Limits: max_user_procs=$max_procs  max_fds=$max_fds  local_port_range=$port_range"
    echo "$max_procs"
}

# ---- Port management ----
ensure_port_free() {
    local port=$1
    fuser -k ${port}/tcp 2>/dev/null || true
    sleep 0.2
    # double-check
    if ss -tlnp 2>/dev/null | grep -q ":${port} "; then
        log_error "Port $port is still in use after kill attempt"
        return 1
    fi
}

wait_for_port_ready() {
    local port=$1
    local timeout=${2:-10}
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        if ss -tlnp 2>/dev/null | grep -q ":${port} "; then
            return 0
        fi
        sleep 0.2
        elapsed=$((elapsed + 1))
    done
    return 1
}

find_free_port() {
    local port
    while true; do
        port=$(( (RANDOM % 20000) + 10000 ))
        if ! ss -tlnp 2>/dev/null | grep -q ":${port} "; then
            echo "$port"
            return 0
        fi
    done
}

# ---- Server lifecycle ----
SERVER_PID=""

start_server() {
    local mode=$1    # fork | pool | select
    local ip=$2
    local port=$3
    local csv_path=${4:-"data/users.csv"}

    ensure_port_free "$port"

    cd "$PROJECT_DIR"

    # Ensure correct users.csv for this test
    if [ "$csv_path" != "data/users.csv" ]; then
        cp data/users.csv data/users.csv.bench_bak 2>/dev/null || true
        cp "$csv_path" data/users.csv
    fi

    case "$mode" in
        fork)
            ./mini_web_server fork "$ip" "$port" > /tmp/bench_${mode}_${port}.stdout 2>&1 &
            ;;
        pool)
            ./mini_web_server pool "$ip" "$port" > /tmp/bench_${mode}_${port}.stdout 2>&1 &
            ;;
        select)
            ./mini_web_server select "$ip" "$port" > /tmp/bench_${mode}_${port}.stdout 2>&1 &
            ;;
        *)
            log_error "Unknown mode: $mode"
            return 1
            ;;
    esac

    SERVER_PID=$!
    echo "$SERVER_PID" > /tmp/bench_server_${port}.pid

    if ! wait_for_port_ready "$port" 10; then
        log_error "Server ($mode) failed to start on $ip:$port within 10s"
        cat /tmp/bench_${mode}_${port}.stdout
        return 1
    fi

    log_info "Server started: mode=$mode pid=$SERVER_PID http://$ip:$port"
    return 0
}

stop_server() {
    local port=$1
    local pid_file="/tmp/bench_server_${port}.pid"

    if [ -f "$pid_file" ]; then
        local pid
        pid=$(cat "$pid_file")
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            # graceful SIGINT
            kill -INT "$pid" 2>/dev/null || true
            # wait up to 3s for graceful exit
            for i in $(seq 1 30); do
                if ! kill -0 "$pid" 2>/dev/null; then
                    break
                fi
                sleep 0.1
            done
            # force kill if still alive
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
        rm -f "$pid_file"
    fi

    ensure_port_free "$port"
    rm -f /tmp/bench_*_${port}.stdout

    # Restore original users.csv if we backed it up
    if [ -f "$PROJECT_DIR/data/users.csv.bench_bak" ]; then
        mv "$PROJECT_DIR/data/users.csv.bench_bak" "$PROJECT_DIR/data/users.csv"
    fi

    log_info "Server stopped (port=$port)"
}

# ---- pidstat collector ----
start_pidstat() {
    local mode=$1
    local output_file=$2
    local parent_pid=$3

    # Find all mini_web_server processes to aggregate
    pidstat -C mini_web_server -u -r -w 1 2>/dev/null > "$output_file" &
    echo $!
}

stop_pidstat() {
    local pid=$1
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# ---- wrk2 wrapper ----
run_wrk2_benchmark() {
    local port=$1
    local conn=$2
    local duration=$3
    local endpoint=$4
    local output_file=$5
    local rate=${6:-1000000}
    local threads
    threads=$(nproc)
    [ "$threads" -gt 8 ] && threads=8

    log_info "wrk2: -t$threads -c$conn -d${duration}s -R$rate $endpoint"

    wrk2 -t"$threads" -c"$conn" -d"${duration}s" -R"$rate" --latency \
        "http://127.0.0.1:${port}${endpoint}" > "$output_file" 2>&1

    local rc=$?
    if [ $rc -ne 0 ]; then
        log_warn "wrk2 exited with code $rc (may indicate timeouts/errors)"
    fi
    return $rc
}

# ---- CSV helpers ----
csv_header_perf() {
    echo "mode,concurrency,run,qps,avg_latency_ms,max_latency_ms,p50_ms,p75_ms,p90_ms,p95_ms,p99_ms,p99.9_ms,total_requests,duration_s,socket_errors,timeout_errors,cpu_avg_pct,rss_avg_kb,cswch_avg_per_sec"
}

csv_header_long() {
    echo "timestamp,mode,concurrency,cpu_pct,rss_kb,vsz_kb,cswch_per_sec,nvcswch_per_sec,process_count,thread_count"
}
