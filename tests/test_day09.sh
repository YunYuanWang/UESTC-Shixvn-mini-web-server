#!/usr/bin/env bash
set -e

# ================================================================
# test_day09.sh — select I/O multiplexing server tests (v0.9)
#
# The select server is single-threaded and event-driven using select().
# It handles multiple connections by monitoring all fds — when one
# fd is ready, it reads, processes, responds, and closes.  Because
# request processing is synchronous, slow endpoints serialize.
#
# Tests:
#   1. Basic HTTP endpoints (hello, users, POST, DELETE, 404)
#   2. 20 concurrent fast /hello requests
#   3. 10 concurrent slow /sleep/100 requests (demonstrates serialization)
#   4. Server handles 50 concurrent fast requests
#   5. Log format check
# ================================================================

HOST="127.0.0.1"
PORT="9090"
BASE="http://${HOST}:${PORT}"

# Helper: wait for specific background pids only
wait_pids() {
    for _pid in "$@"; do
        wait "$_pid" 2>/dev/null || true
    done
}

# Helper: start select server on given port
start_server() {
    local host=${1:-127.0.0.1}
    local port=${2:-9090}
    # Ensure port is free
    fuser -k ${port}/tcp 2>/dev/null || true
    sleep 0.2
    ./mini_web_server select "$host" "$port" &
    SERVER_PID=$!
    sleep 0.3
}

# Helper: stop server
stop_server() {
    local pid=${1:-$SERVER_PID}
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        sleep 0.2
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

# backup data
cp data/users.csv data/users.csv.bak

# build
make clean > /dev/null 2>&1
make > /dev/null 2>&1

# cleanup all leftovers
pkill -f "mini_web_server" 2>/dev/null || true
sleep 0.3

# ================================================================
echo "=== Test 1: basic HTTP endpoints ==="
start_server 127.0.0.1 9090

# GET /hello
RESP=$(curl -s "${BASE}/hello")
echo "$RESP" | grep -qF "Hello, Web!" && echo "PASS: GET /hello" || { echo "FAIL: GET /hello"; exit 1; }

# GET /users/ZhangSan
RESP=$(curl -s "${BASE}/users/ZhangSan")
echo "$RESP" | grep -qF "name: ZhangSan" && echo "PASS: GET /users/ZhangSan" || { echo "FAIL"; exit 1; }

# POST /users
RESP=$(curl -s -X POST -d "Day09,pass999,2000,01,139,day09@t.com" "${BASE}/users")
echo "$RESP" | grep -qF "ADDED" && echo "PASS: POST /users" || { echo "FAIL"; exit 1; }

# DELETE /users/Day09
RESP=$(curl -s -X DELETE "${BASE}/users/Day09")
echo "$RESP" | grep -qF "DELETED" && echo "PASS: DELETE /users/Day09" || { echo "FAIL"; exit 1; }

# 404
RESP=$(curl -s "${BASE}/no/such/path")
echo "$RESP" | grep -qF "404 Not Found" && echo "PASS: GET /no/such/path -> 404" || { echo "FAIL"; exit 1; }

stop_server $SERVER_PID

# ================================================================
echo ""
echo "=== Test 2: 20 concurrent fast /hello requests ==="
rm -f logs/server.log
start_server 127.0.0.1 9091
BASE2="http://127.0.0.1:9091"

PIDS=""
for i in $(seq 1 20); do
    curl -s -o /tmp/d9_t2_${i}.out "${BASE2}/hello" &
    PIDS="$PIDS $!"
done
wait_pids $PIDS

ok=0
for i in $(seq 1 20); do
    grep -qF "Hello, Web!" /tmp/d9_t2_${i}.out 2>/dev/null && ok=$((ok+1))
    rm -f /tmp/d9_t2_${i}.out
done

if [ "$ok" -eq 20 ]; then
    echo "PASS: 20/20 concurrent /hello all returned 200 OK"
else
    echo "FAIL: only $ok/20 succeeded"
    exit 1
fi

stop_server $SERVER_PID

# ================================================================
echo ""
echo "=== Test 3: 10 concurrent slow /sleep/100 requests ==="
echo "Select server processes slow requests sequentially (single-threaded)."
echo "10 requests × 100ms ≈ 1s+ wall-clock time."
rm -f logs/server.log
start_server 127.0.0.1 9092
BASE3="http://127.0.0.1:9092"

START_TIME=$(date +%s)

PIDS=""
for i in $(seq 1 10); do
    curl -s -o /tmp/d9_t3_${i}.out "${BASE3}/sleep/100" &
    PIDS="$PIDS $!"
done
wait_pids $PIDS

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

ok=0
for i in $(seq 1 10); do
    grep -qF "Slept 100 ms" /tmp/d9_t3_${i}.out 2>/dev/null && ok=$((ok+1))
    rm -f /tmp/d9_t3_${i}.out
done

echo "OK: $ok/10"
echo "Elapsed: ${ELAPSED}s"

if [ "$ok" -eq 10 ]; then
    echo "PASS: all 10 slow requests returned 200 OK"
else
    echo "FAIL: only $ok/10 succeeded"
    exit 1
fi

# Select server is single-threaded → slow requests serialize
# 10 × 100ms = 1000ms, plus overhead → expect >= ~1s
if [ "$ELAPSED" -ge 1 ]; then
    echo "PASS: elapsed ${ELAPSED}s >= 1s (consistent with serial execution)"
else
    echo "FAIL: elapsed ${ELAPSED}s < 1s — unexpected"
    exit 1
fi

stop_server $SERVER_PID

# ================================================================
echo ""
echo "=== Test 4: 50 concurrent fast /hello requests ==="
echo "Select server handles many connections via fd_set multiplexing."
rm -f logs/server.log
start_server 127.0.0.1 9093
BASE4="http://127.0.0.1:9093"

PIDS=""
for i in $(seq 1 50); do
    curl -s -o /tmp/d9_t4_${i}.out "${BASE4}/hello" &
    PIDS="$PIDS $!"
done
wait_pids $PIDS

ok=0
for i in $(seq 1 50); do
    grep -qF "Hello, Web!" /tmp/d9_t4_${i}.out 2>/dev/null && ok=$((ok+1))
    rm -f /tmp/d9_t4_${i}.out
done

echo "OK: $ok/50"
if [ "$ok" -eq 50 ]; then
    echo "PASS: all 50 concurrent /hello returned 200 OK"
else
    echo "FAIL: only $ok/50 succeeded"
    exit 1
fi

stop_server $SERVER_PID

# ================================================================
echo ""
echo "=== Test 5: log format ==="
rm -f logs/server.log
start_server 127.0.0.1 9094
BASE5="http://127.0.0.1:9094"

curl -s "${BASE5}/hello" > /dev/null
curl -s "${BASE5}/users/ZhangSan" > /dev/null

stop_server $SERVER_PID

if grep -qF "SelectServer" logs/server.log 2>/dev/null; then
    echo "PASS: log contains 'SelectServer' entries"
else
    echo "FAIL: missing SelectServer log entries"
    exit 1
fi

if grep -q "request:" logs/server.log 2>/dev/null; then
    echo "PASS: log contains request method+path"
else
    echo "FAIL: missing request log"
    exit 1
fi

if grep -q "response:" logs/server.log 2>/dev/null; then
    echo "PASS: log contains response status"
else
    echo "FAIL: missing response log"
    exit 1
fi

echo ""
echo "=== Log sample ==="
grep -E "(SelectServer|request:|response:)" logs/server.log 2>/dev/null || true

# ================================================================
# Cleanup
# ================================================================
rm -f logs/server.log
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY09 PASS"
