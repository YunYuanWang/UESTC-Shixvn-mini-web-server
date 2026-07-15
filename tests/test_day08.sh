#!/usr/bin/env bash
set -e

# ================================================================
# test_day08.sh — multi-threaded TCP/HTTP server tests (v0.8)
#
# Tests:
#   1. Pool server handles 5 concurrent GET /hello requests
#   2. Pool server handles GET /users/<name>
#   3. Pool server handles POST /users
#   4. Pool server handles DELETE /users/<name>
#   5. Pool server handles 404 NOT FOUND
#   6. Pool server exits cleanly after MAX_CLIENTS
#   7. Pool server logs include worker thread numbers
#   8. Queue congestion: tasks wait when all workers are busy
# ================================================================

HOST="127.0.0.1"
PORT="8080"
BASE="http://${HOST}:${PORT}"

# Helper: kill server gracefully
stop_server() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

# Helper: start server, wait for it to be ready
start_server() {
    ./mini_web_server pool &
    SERVER_PID=$!
    sleep 0.5
}

# backup data
cp data/users.csv data/users.csv.bak

# build
make clean > /dev/null 2>&1
make > /dev/null 2>&1

# cleanup leftover processes from previous runs
pkill -f "mini_web_server pool" 2>/dev/null || true
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
    curl -s "${BASE}/hello" > /tmp/test_day08_${i}.out 2>&1 &
done
wait  # wait for all curl processes

# Wait for server to exit naturally (after MAX_CLIENTS=5)
sleep 1
wait $SERVER_PID 2>/dev/null || true

# Verify all 5 responses contain "Hello, Web!"
passed=0
for i in 1 2 3 4 5; do
    if grep -qF "Hello, Web!" /tmp/test_day08_${i}.out 2>/dev/null; then
        passed=$((passed + 1))
    else
        echo "FAIL: request $i did not return Hello, Web!"
        cat /tmp/test_day08_${i}.out
    fi
    rm -f /tmp/test_day08_${i}.out
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
RESP=$(curl -s -X POST -d "Day08Test,pass888,20000801,010-88888888,13900000000,day08@test.com" "${BASE}/users" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "ADDED"
echo "PASS: POST /users"

# ================================================================
echo "=== Test 4: DELETE /users/Day08Test ==="
start_server
RESP=$(curl -s -X DELETE "${BASE}/users/Day08Test" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "DELETED"
echo "PASS: DELETE /users/Day08Test"

# ================================================================
echo "=== Test 5: GET /not-exist (404) ==="
start_server
RESP=$(curl -s "${BASE}/not-exist" 2>&1 || true)
stop_server $SERVER_PID
echo "$RESP" | grep -F "404 Not Found"
echo "PASS: GET /not-exist"

# ================================================================
echo "=== Test 6: server exits after MAX_CLIENTS ==="
start_server

# Send exactly MAX_CLIENTS (5) requests
for i in 1 2 3 4 5; do
    curl -s "${BASE}/hello" > /dev/null 2>&1 &
done
wait

# Server should exit naturally within a few seconds
exited=0
for i in $(seq 1 10); do
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        exited=1
        break
    fi
    sleep 0.5
done

if [ "$exited" -eq 1 ]; then
    echo "PASS: server exited after MAX_CLIENTS"
else
    echo "FAIL: server did not exit after MAX_CLIENTS"
    stop_server $SERVER_PID
    exit 1
fi

# ================================================================
echo "=== Test 7: log contains worker thread info ==="
# Run one more server to produce logs
start_server
curl -s "${BASE}/hello" > /dev/null 2>&1 || true
stop_server $SERVER_PID

if grep -qE '\[Worker-[0-9]+\]' logs/server.log 2>/dev/null; then
    echo "PASS: log contains [Worker-N] entries"
else
    echo "FAIL: log does not contain [Worker-N] entries"
    exit 1
fi

if grep -qF "PoolServer" logs/server.log 2>/dev/null; then
    echo "PASS: log contains PoolServer entries"
else
    echo "FAIL: log does not contain PoolServer entries"
    exit 1
fi

# ================================================================
echo "=== Test 8: queue congestion — all workers busy ==="
echo "Sending 5 concurrent slow requests (/sleep/500, 500ms each)"
echo "against 4 workers — the 5th task must queue."
echo ""

# Start server with fresh log
rm -f logs/server.log
./mini_web_server pool &
SERVER_PID=$!
sleep 0.5

# Record start time
START_TIME=$(date +%s)

# Send 5 concurrent slow requests
for i in 1 2 3 4 5; do
    curl -s -o /tmp/test_queue_${i}.out "${BASE}/sleep/500" 2>&1 &
done
wait

# Record end time
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

# Allow server to exit naturally
sleep 1
wait $SERVER_PID 2>/dev/null || true

# --- Verify all 5 responses succeeded ---
passed=0
for i in 1 2 3 4 5; do
    if grep -qF "Slept 500 ms" /tmp/test_queue_${i}.out 2>/dev/null; then
        passed=$((passed + 1))
    else
        echo "FAIL: request $i did not return 'Slept 500 ms'"
        cat /tmp/test_queue_${i}.out
    fi
    rm -f /tmp/test_queue_${i}.out
done

if [ "$passed" -eq 5 ]; then
    echo "PASS: All 5 slow requests returned 200 OK ($passed/5)"
else
    echo "FAIL: only $passed/5 slow requests succeeded"
    exit 1
fi

# --- Elapsed time: should be >= 1s (two 500ms batches) ---
echo "Elapsed wall-clock time: ${ELAPSED}s"
if [ "$ELAPSED" -ge 1 ]; then
    echo "PASS: total time >= 1s (consistent with 2 batches of 500ms)"
else
    echo "FAIL: total time < 1s — tasks may not have queued"
    exit 1
fi

# --- Log analysis ---
ENQUEUED=$(grep -cF "task #" logs/server.log 2>/dev/null || echo 0)
PROCESSED=$(grep -cF "processing client_fd=" logs/server.log 2>/dev/null || echo 0)
WORKERS_USED=$(grep -oE '\[Worker-[0-9]+\]' logs/server.log 2>/dev/null | sort -u | wc -l)

echo "Tasks enqueued: $ENQUEUED"
echo "Tasks processed: $PROCESSED"
echo "Distinct workers used: $WORKERS_USED"

if [ "$ENQUEUED" -ge 5 ] && [ "$PROCESSED" -ge 5 ]; then
    echo "PASS: all tasks enqueued and processed"
else
    echo "FAIL: expected >=5 enqueued/processed, got $ENQUEUED/$PROCESSED"
    exit 1
fi

if [ "$WORKERS_USED" -ge 2 ]; then
    echo "PASS: multiple workers participated ($WORKERS_USED workers)"
else
    echo "FAIL: only $WORKERS_USED worker(s) used — expected >=2"
    exit 1
fi

# --- Log snapshot ---
echo ""
echo "=== Log Snapshot (key events) ==="
grep -E "(task #|processing|sleeping|closed)" logs/server.log 2>/dev/null || true

echo ""
echo "  Queue behavior:"
echo "    t=0ms:  4 workers dequeue tasks #1-4 immediately"
echo "    t=0ms:  Task #5 waits in queue (all workers busy)"
echo "    t=500ms:Worker finishes, dequeues task #5"
echo "    t=1s:   All done — wall clock confirms queuing"

# ================================================================
# Cleanup
# ================================================================
rm -f logs/server.log
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY08 PASS"
