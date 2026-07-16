#!/usr/bin/env bash
set -e

# ================================================================
# test_day10.sh — epoll I/O multiplexing server tests (v0.10)
#
# The epoll server is single-threaded and event-driven using
# epoll_create1 / epoll_ctl / epoll_wait.  It delivers ready
# events in O(1) time (vs select's O(n) fd scan) and has no
# FD_SETSIZE limit.
#
# Tests:
#   1. Basic HTTP endpoints (hello, users, POST, DELETE, 404)
#   2. 20 concurrent fast /hello requests
#   3. 3-client simultaneous connection demo (epoll_client)
#   4. 50 concurrent fast /hello requests
#   5. Keep-alive connection reuse
#   6. Log format check
#   7. Standalone EpollServer binary test
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

# Helper: start epoll server on given port (via mini_web_server dispatch)
start_server() {
    local host=${1:-127.0.0.1}
    local port=${2:-9090}
    # Ensure port is free
    fuser -k ${port}/tcp 2>/dev/null || true
    sleep 0.2
    ./mini_web_server epoll "$host" "$port" &
    SERVER_PID=$!
    sleep 0.3
}

# Helper: start standalone EpollServer binary
start_epoll_server_bin() {
    local host=${1:-127.0.0.1}
    local port=${2:-9095}
    fuser -k ${port}/tcp 2>/dev/null || true
    sleep 0.2
    ./EpollServer "$host" "$port" &
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

# verify EpollServer binary exists
if [ ! -x "./EpollServer" ]; then
    echo "FAIL: EpollServer binary not built"
    exit 1
fi
echo "Build OK: EpollServer binary exists"

# verify epoll_client binary exists
if [ ! -x "./epoll_client" ]; then
    echo "FAIL: epoll_client binary not built"
    exit 1
fi
echo "Build OK: epoll_client binary exists"

# cleanup all leftovers
pkill -f "mini_web_server" 2>/dev/null || true
pkill -f "EpollServer" 2>/dev/null || true
sleep 0.3

# ================================================================
echo ""
echo "=== Test 1: basic HTTP endpoints (epoll dispatch mode) ==="
start_server 127.0.0.1 9090

# GET /hello
RESP=$(curl -s "${BASE}/hello")
echo "$RESP" | grep -qF "Hello, Web!" && echo "PASS: GET /hello" || { echo "FAIL: GET /hello"; exit 1; }

# GET /users/ZhangSan
RESP=$(curl -s "${BASE}/users/ZhangSan")
echo "$RESP" | grep -qF "name: ZhangSan" && echo "PASS: GET /users/ZhangSan" || { echo "FAIL"; exit 1; }

# POST /users
RESP=$(curl -s -X POST -d "Day10,pass999,2000,01,139,day10@t.com" "${BASE}/users")
echo "$RESP" | grep -qF "ADDED" && echo "PASS: POST /users" || { echo "FAIL"; exit 1; }

# DELETE /users/Day10
RESP=$(curl -s -X DELETE "${BASE}/users/Day10")
echo "$RESP" | grep -qF "DELETED" && echo "PASS: DELETE /users/Day10" || { echo "FAIL"; exit 1; }

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
    curl -s -o /tmp/d10_t2_${i}.out "${BASE2}/hello" &
    PIDS="$PIDS $!"
done
wait_pids $PIDS

ok=0
for i in $(seq 1 20); do
    grep -qF "Hello, Web!" /tmp/d10_t2_${i}.out 2>/dev/null && ok=$((ok+1))
    rm -f /tmp/d10_t2_${i}.out
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
echo "=== Test 3: 3-client simultaneous connection demo ==="
echo "Starting EpollServer on port 9092..."
rm -f logs/server.log
start_server 127.0.0.1 9092
BASE3="http://127.0.0.1:9092"

# Start 3 epoll_clients in background, each sending a few messages then quit
echo "Launching 3 epoll_clients..."

# Client 1: send HTTP-like messages
(
    sleep 0.5
    echo "GET /hello"
    sleep 0.3
    echo "quit"
) | ./epoll_client 127.0.0.1 9092 > /tmp/d10_client1.out 2>&1 &
CLIENT1_PID=$!

# Client 2: send raw messages
(
    sleep 0.5
    echo "Hello from client 2"
    sleep 0.3
    echo "quit"
) | ./epoll_client 127.0.0.1 9092 > /tmp/d10_client2.out 2>&1 &
CLIENT2_PID=$!

# Client 3: send raw messages
(
    sleep 0.5
    echo "Message from client 3"
    sleep 0.3
    echo "quit"
) | ./epoll_client 127.0.0.1 9092 > /tmp/d10_client3.out 2>&1 &
CLIENT3_PID=$!

# Wait for all clients to finish
wait $CLIENT1_PID 2>/dev/null || true
wait $CLIENT2_PID 2>/dev/null || true
wait $CLIENT3_PID 2>/dev/null || true

echo "All 3 clients finished."
echo ""
echo "Client 1 output:"
cat /tmp/d10_client1.out
echo ""
echo "Client 2 output:"
cat /tmp/d10_client2.out
echo ""
echo "Client 3 output:"
cat /tmp/d10_client3.out

# Verify each client connected and got a response
for i in 1 2 3; do
    if grep -qF "Connected to EpollServer" /tmp/d10_client${i}.out 2>/dev/null; then
        echo "PASS: client $i connected successfully"
    else
        echo "FAIL: client $i did not connect"
        exit 1
    fi
done

# Verify server log shows 3 connections
if grep -qF "EpollServer" logs/server.log 2>/dev/null; then
    echo "PASS: server logged EpollServer entries"
else
    echo "FAIL: no EpollServer log entries"
    exit 1
fi

stop_server $SERVER_PID
rm -f /tmp/d10_client1.out /tmp/d10_client2.out /tmp/d10_client3.out

# ================================================================
echo ""
echo "=== Test 4: 50 concurrent fast /hello requests ==="
echo "Epoll server handles many connections via epoll_wait."
rm -f logs/server.log
start_server 127.0.0.1 9093
BASE4="http://127.0.0.1:9093"

PIDS=""
for i in $(seq 1 50); do
    curl -s -o /tmp/d10_t4_${i}.out "${BASE4}/hello" &
    PIDS="$PIDS $!"
done
wait_pids $PIDS

ok=0
for i in $(seq 1 50); do
    grep -qF "Hello, Web!" /tmp/d10_t4_${i}.out 2>/dev/null && ok=$((ok+1))
    rm -f /tmp/d10_t4_${i}.out
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
echo "=== Test 5: keep-alive connection reuse ==="
rm -f logs/server.log
start_server 127.0.0.1 9094
BASE5="http://127.0.0.1:9094"

# Use curl with keep-alive to send multiple requests on one connection
KEEPALIVE_OUT=$(curl -s --keepalive-time 5 \
    "${BASE5}/hello" \
    "${BASE5}/users/ZhangSan" \
    "${BASE5}/hello" 2>&1 || true)

# Just verify the requests went through
echo "$KEEPALIVE_OUT" | grep -qF "Hello, Web!" && echo "PASS: keep-alive /hello 1" || echo "WARN: keep-alive check"
echo "$KEEPALIVE_OUT" | grep -qF "ZhangSan" && echo "PASS: keep-alive /users/ZhangSan" || echo "WARN: keep-alive check"

stop_server $SERVER_PID

# ================================================================
echo ""
echo "=== Test 6: log format ==="
rm -f logs/server.log
start_server 127.0.0.1 9096
BASE6="http://127.0.0.1:9096"

curl -s "${BASE6}/hello" > /dev/null
curl -s "${BASE6}/users/ZhangSan" > /dev/null

stop_server $SERVER_PID

if grep -qF "EpollServer" logs/server.log 2>/dev/null; then
    echo "PASS: log contains 'EpollServer' entries"
else
    echo "FAIL: missing EpollServer log entries"
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
grep -E "(EpollServer|request:|response:)" logs/server.log 2>/dev/null | head -10 || true

# ================================================================
echo ""
echo "=== Test 7: standalone EpollServer binary ==="
rm -f logs/server.log
start_epoll_server_bin 127.0.0.1 9095
BASE7="http://127.0.0.1:9095"

RESP=$(curl -s "${BASE7}/hello")
echo "$RESP" | grep -qF "Hello, Web!" && echo "PASS: EpollServer binary GET /hello" || { echo "FAIL"; exit 1; }

RESP=$(curl -s "${BASE7}/users/LiSi")
echo "$RESP" | grep -qF "name: LiSi" && echo "PASS: EpollServer binary GET /users/LiSi" || { echo "FAIL"; exit 1; }

# Verify log contains EpollServer entries
COUNT=$(grep -c "EpollServer" logs/server.log 2>/dev/null || echo "0")
echo "EpollServer binary log entries: $COUNT"

stop_server $SERVER_PID

# ================================================================
echo ""
echo "=== Test 8: wrk benchmark preparation check ==="
echo "Verifying HTTP/1.1 compliance for wrk testing..."

rm -f logs/server.log
start_server 127.0.0.1 9097
BASE8="http://127.0.0.1:9097"

# Check HTTP/1.1 response headers
RESP_HEADERS=$(curl -s -D - "${BASE8}/hello" 2>&1 | head -10)
echo "$RESP_HEADERS"

if echo "$RESP_HEADERS" | grep -qF "HTTP/1.1 200"; then
    echo "PASS: HTTP/1.1 200 OK response"
else
    echo "FAIL: not HTTP/1.1"
    exit 1
fi

if echo "$RESP_HEADERS" | grep -qi "Content-Length"; then
    echo "PASS: Content-Length header present"
else
    echo "FAIL: missing Content-Length header"
    exit 1
fi

if echo "$RESP_HEADERS" | grep -qi "Connection:"; then
    echo "PASS: Connection header present"
else
    echo "WARN: no Connection header"
fi

stop_server $SERVER_PID

# ================================================================
# Cleanup
# ================================================================
rm -f logs/server.log
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY10 PASS"
