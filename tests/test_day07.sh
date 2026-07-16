#!/usr/bin/env bash
set -e

# ================================================================
# test_day07.sh — multi-process TCP/HTTP server tests (v0.7)
#
# Tests:
#   1. Fork server handles 5 concurrent GET /hello requests
#   2. Fork server handles GET /users/<name>
#   3. Fork server handles POST /users
#   4. Fork server handles DELETE /users/<name>
#   5. Fork server handles 404 NOT FOUND
#   6. Fork server handles SIGINT graceful shutdown
# ================================================================

HOST="127.0.0.1"
PORT="8080"
BASE="http://${HOST}:${PORT}"

# Helper: kill server gracefully via SIGINT
stop_server() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        sleep 0.3
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

# Helper: start server, wait for it to be ready
start_server() {
    ./mini_web_server fork &
    SERVER_PID=$!
    sleep 0.5
}

# backup data
cp data/users.csv data/users.csv.bak

# build
make clean > /dev/null 2>&1
make > /dev/null 2>&1

# cleanup leftover processes from previous runs
pkill -f "mini_web_server fork" 2>/dev/null || true
sleep 0.3
# ensure port 8080 is free
while ss -tlnp 2>/dev/null | grep -q ":8080 "; do
    echo "Waiting for port 8080 to be free..."
    sleep 0.5
done

# ================================================================
echo "=== Test 1: 5 concurrent GET /hello ==="
start_server

# Send 5 concurrent requests, capture outputs
for i in 1 2 3 4 5; do
    curl -s "${BASE}/hello" > /tmp/test_day07_${i}.out 2>&1 &
done
wait  # wait for all curl processes

# Stop server gracefully (no longer auto-exits after MAX_CLIENTS)
sleep 0.5
stop_server $SERVER_PID

# Verify all 5 responses contain "Hello, Web!"
passed=0
for i in 1 2 3 4 5; do
    if grep -qF "Hello, Web!" /tmp/test_day07_${i}.out 2>/dev/null; then
        passed=$((passed + 1))
    else
        echo "FAIL: request $i did not return Hello, Web!"
        cat /tmp/test_day07_${i}.out
    fi
    rm -f /tmp/test_day07_${i}.out
done

if [ "$passed" -eq 5 ]; then
    echo "PASS: 5 concurrent GET /hello ($passed/5 OK)"
else
    echo "FAIL: only $passed/5 requests succeeded"
    exit 1
fi

# ================================================================
echo "=== Test 2: GET /users/ZhangSan ==="
start_server
RESP=$(curl -s "${BASE}/users/ZhangSan" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "name: ZhangSan"
echo "$RESP" | grep -F "email: zhangsan@stdmail.com"
echo "PASS: GET /users/ZhangSan"

# ================================================================
echo "=== Test 3: POST /users (add user) ==="
start_server
RESP=$(curl -s -X POST -d "Day07Test,pass777,20000701,010-77777777,13900000000,day07@test.com" "${BASE}/users" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "ADDED"
echo "PASS: POST /users"

# ================================================================
echo "=== Test 4: DELETE /users/Day07Test ==="
start_server
RESP=$(curl -s -X DELETE "${BASE}/users/Day07Test" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "DELETED"
echo "PASS: DELETE /users/Day07Test"

# ================================================================
echo "=== Test 5: GET /not-exist (404) ==="
start_server
RESP=$(curl -s "${BASE}/not-exist" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "404 Not Found"
echo "PASS: GET /not-exist"

# ================================================================
echo "=== Test 6: SIGINT graceful shutdown ==="
start_server

# Send some requests
for i in 1 2 3 4 5; do
    curl -s "${BASE}/hello" > /dev/null 2>&1 &
done
wait

# Server should be still running (no auto-exit)
if kill -0 $SERVER_PID 2>/dev/null; then
    echo "PASS: server still running after 5 requests (no auto-exit)"
else
    echo "FAIL: server already exited"
    exit 1
fi

# Send SIGINT — should shut down gracefully
stop_server $SERVER_PID
echo "PASS: SIGINT graceful shutdown"

# ================================================================
# Cleanup
# ================================================================
rm -f logs/server.log
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY07 PASS"
