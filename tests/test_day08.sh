#!/usr/bin/env bash
set -e

# ================================================================
# test_day08.sh — dynamic thread pool TCP/HTTP server tests (v0.8)
#
# Pool config: core=2 max=8 queue_cap=128 max_clients=30
#
# Tests:
#   1. Pool server handles 5 concurrent GET /hello requests
#   2. Pool server handles GET /users/<name>
#   3. Pool server handles POST /users
#   4. Pool server handles DELETE /users/<name>
#   5. Pool server handles 404 NOT FOUND
#   6. Pool server exits cleanly after POOL_MAX_CLIENTS
#   7. Log contains worker thread info + pool lifecycle events
#   8. Dynamic scaling: scale-up under load, scale-down on idle
# ================================================================

HOST="127.0.0.1"
PORT="8080"
BASE="http://${HOST}:${PORT}"

stop_server() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        sleep 0.2
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

start_server() {
    # Ensure any leftover server is killed first
    if ss -tlnp 2>/dev/null | grep -q ":8080 "; then
        fuser -k 8080/tcp 2>/dev/null || true
        sleep 0.3
    fi
    ./mini_web_server pool &
    SERVER_PID=$!
    sleep 0.5
}

# backup data
cp data/users.csv data/users.csv.bak

# build
make clean > /dev/null 2>&1
make > /dev/null 2>&1

# cleanup leftovers
pkill -f "mini_web_server pool" 2>/dev/null || true
sleep 0.3
while ss -tlnp 2>/dev/null | grep -q ":8080 "; do
    echo "Waiting for port 8080 to be free..."
    sleep 0.5
done

# ================================================================
echo "=== Test 1: 5 concurrent GET /hello ==="
start_server

for i in 1 2 3 4 5; do
    curl -s "${BASE}/hello" > /tmp/test_d8_${i}.out 2>&1 &
    CURL_PIDS="$CURL_PIDS $!"
done
for pid in $CURL_PIDS; do wait $pid 2>/dev/null || true; done

sleep 0.2
stop_server $SERVER_PID

passed=0
for i in 1 2 3 4 5; do
    if grep -qF "Hello, Web!" /tmp/test_d8_${i}.out 2>/dev/null; then
        passed=$((passed + 1))
    else
        echo "FAIL: request $i did not return Hello, Web!"
        cat /tmp/test_d8_${i}.out
    fi
    rm -f /tmp/test_d8_${i}.out
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
echo "=== Test 6: server exits after POOL_MAX_CLIENTS (30) ==="
start_server

# Send exactly 30 requests to trigger natural exit
for i in $(seq 1 30); do
    curl -s "${BASE}/hello" > /dev/null 2>&1 &
done
wait

# Server should exit naturally
exited=0
for i in $(seq 1 15); do
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        exited=1
        break
    fi
    sleep 0.5
done

if [ "$exited" -eq 1 ]; then
    echo "PASS: server exited after POOL_MAX_CLIENTS"
else
    echo "FAIL: server did not exit after POOL_MAX_CLIENTS"
    stop_server $SERVER_PID
    exit 1
fi

# ================================================================
echo "=== Test 7: log contains pool lifecycle events ==="
start_server
sleep 0.3
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

if grep -qE "created.*core=2.*max=8" logs/server.log 2>/dev/null; then
    echo "PASS: pool created with core=2 max=8"
else
    echo "FAIL: pool creation log missing or wrong params"
    exit 1
fi

# ================================================================
echo "=== Test 8a: dynamic scale-up under load ==="
echo "Sending 30 concurrent slow requests (/sleep/200, 200ms each)"
echo "Core=2 workers, max=8.  Should scale up to 8 workers."
echo ""

rm -f logs/server.log
./mini_web_server pool &
SERVER_PID=$!
sleep 0.5

START_TIME=$(date +%s)

for i in $(seq 1 30); do
    curl -s -o /tmp/test_d8_s${i}.out "${BASE}/sleep/200" 2>&1 &
done
wait

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

# Wait for natural exit after 30 clients
for i in $(seq 1 20); do
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "Server exited after ~$((i*500))ms"
        break
    fi
    sleep 0.5
done
kill -9 $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# --- Verify all 30 succeeded ---
passed=0
for i in $(seq 1 30); do
    if grep -qF "Slept 200 ms" /tmp/test_d8_s${i}.out 2>/dev/null; then
        passed=$((passed + 1))
    fi
    rm -f /tmp/test_d8_s${i}.out
done
echo "Requests succeeded: $passed/30"
if [ "$passed" -eq 30 ]; then
    echo "PASS: all 30 slow requests returned 200 OK"
else
    echo "FAIL: only $passed/30 succeeded"
    exit 1
fi

# --- Pool scaled up beyond core size ---
SCALE_UPS=$(grep -c "scaled up" logs/server.log 2>/dev/null)
echo "Scale-up events: $SCALE_UPS (including initial core workers)"
if [ "$SCALE_UPS" -ge 3 ]; then
    echo "PASS: pool scaled up ($SCALE_UPS events)"
else
    echo "FAIL: only $SCALE_UPS scale-up events"
    exit 1
fi

# --- Peak exceeded core ---
PEAK=$(grep "peak workers" logs/server.log 2>/dev/null | grep -oE '[0-9]+' | tail -1)
echo "Peak workers: $PEAK"
if [ "$PEAK" -gt 2 ]; then
    echo "PASS: peak workers ($PEAK) > core (2)"
else
    echo "FAIL: peak workers ($PEAK) not > core (2)"
    exit 1
fi

echo "Elapsed wall-clock time: ${ELAPSED}s"

# ================================================================
echo ""
echo "=== Test 8b: dynamic scale-down on idle ==="
echo "Sending 10 requests (under MAX_CLIENTS=30), then waiting for"
echo "extra workers to idle-timeout and scale back to core=2."
echo ""

rm -f logs/server.log
./mini_web_server pool &
SERVER_PID=$!
sleep 0.5

# Send 10 concurrent slow requests (enough to trigger scale-up)
for i in $(seq 1 10); do
    curl -s -o /tmp/test_d8_sd${i}.out "${BASE}/sleep/200" 2>&1 &
    CURL_PIDS="$CURL_PIDS $!"
done
for pid in $CURL_PIDS; do wait $pid 2>/dev/null || true; done

# Now wait for idle timeout (IDLE_TIMEOUT_MS=1s + buffer)
echo "Waiting for idle timeout (extra workers should scale down)..."
sleep 4

# Kill server
kill -INT $SERVER_PID 2>/dev/null || true
sleep 0.3
kill -9 $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# Verify responses
for i in $(seq 1 10); do
    rm -f /tmp/test_d8_sd${i}.out
done

# --- Check scale-down ---
SCALE_DOWNS=$(grep -c "idle timeout" logs/server.log 2>/dev/null)
echo "Scale-down events (idle timeout): $SCALE_DOWNS"
if [ "$SCALE_DOWNS" -ge 1 ]; then
    echo "PASS: extra workers scaled down after going idle"
else
    echo "FAIL: no idle-timeout scale-down events"
    exit 1
fi

# --- Check final worker count back to core ---
FINAL_COUNT=$(grep "now [0-9]* workers" logs/server.log 2>/dev/null | tail -1 | grep -oE '[0-9]+' | tail -1)
echo "Last 'now N workers' count: $FINAL_COUNT"
if [ "$FINAL_COUNT" -eq 2 ]; then
    echo "PASS: worker count returned to core ($FINAL_COUNT)"
else
    echo "FAIL: final worker count $FINAL_COUNT, expected 2 (core)"
    exit 1
fi

# --- Log summary ---
echo ""
echo "=== Pool lifecycle (scale-down test) ==="
grep -E "(created|scaled up|idle timeout|all workers|peak)" logs/server.log 2>/dev/null || true

# ================================================================
echo ""
echo "=== Test 8c: re-scale after scale-down ==="
echo "1st batch: send 10 slow requests → scale up"
echo "Wait for idle timeout → scale down"
echo "2nd batch: send 10 more slow requests → must scale back up"
echo ""

rm -f logs/server.log
./mini_web_server pool &
SERVER_PID=$!
sleep 0.5

# ---- 1st batch: trigger scale-up ----
echo "--- 1st batch (10 requests) ---"
CURL_PIDS=""
for i in $(seq 1 10); do
    curl -s -o /tmp/test_d8_r1_${i}.out "${BASE}/sleep/300" 2>&1 &
    CURL_PIDS="$CURL_PIDS $!"
done
for pid in $CURL_PIDS; do wait $pid 2>/dev/null || true; done

ok1=0
for i in $(seq 1 10); do
    grep -q "Slept 300 ms" /tmp/test_d8_r1_${i}.out 2>/dev/null && ok1=$((ok1+1))
    rm -f /tmp/test_d8_r1_${i}.out
done
echo "1st batch OK: $ok1/10"

# Capture scale-up count after 1st batch
UP_AFTER_1ST=$(grep -c "scaled up" logs/server.log 2>/dev/null)
echo "Scale-up events after 1st batch: $UP_AFTER_1ST"

# ---- Wait for idle timeout ----
echo "Waiting 4s for idle timeout (scale down)..."
sleep 4

DOWN_AFTER_IDLE=$(grep -c "idle timeout" logs/server.log 2>/dev/null)
echo "Scale-down events after idle: $DOWN_AFTER_IDLE"

# Take a snapshot of "now N workers" after scale-down
LAST_COUNT_AFTER_IDLE=$(grep "now [0-9]* workers" logs/server.log 2>/dev/null | tail -1 | grep -oE '[0-9]+' | tail -1)
echo "Worker count after idle: $LAST_COUNT_AFTER_IDLE"

# ---- 2nd batch: should trigger re-scale ----
echo "--- 2nd batch (10 requests) ---"
CURL_PIDS=""
for i in $(seq 1 10); do
    curl -s -o /tmp/test_d8_r2_${i}.out "${BASE}/sleep/300" 2>&1 &
    CURL_PIDS="$CURL_PIDS $!"
done
for pid in $CURL_PIDS; do wait $pid 2>/dev/null || true; done

ok2=0
for i in $(seq 1 10); do
    grep -q "Slept 300 ms" /tmp/test_d8_r2_${i}.out 2>/dev/null && ok2=$((ok2+1))
    rm -f /tmp/test_d8_r2_${i}.out
done
echo "2nd batch OK: $ok2/10"

# Capture total scale-up events (should have increased from 1st batch)
UP_AFTER_2ND=$(grep -c "scaled up" logs/server.log 2>/dev/null)
echo "Scale-up events after 2nd batch: $UP_AFTER_2ND"

# Kill server
kill -INT $SERVER_PID 2>/dev/null || true
sleep 0.3
kill -9 $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# ---- Verifications ----
echo ""
# 8c-1: both batches succeeded
if [ "$ok1" -eq 10 ] && [ "$ok2" -eq 10 ]; then
    echo "PASS: both batches returned 200 OK (10/10 each)"
else
    echo "FAIL: 1st=$ok1/10, 2nd=$ok2/10"
    exit 1
fi

# 8c-2: pool scaled up during 1st batch
if [ "$UP_AFTER_1ST" -ge 3 ]; then
    echo "PASS: 1st batch triggered scale-up ($UP_AFTER_1ST events)"
else
    echo "FAIL: only $UP_AFTER_1ST scale-up events in 1st batch"
    exit 1
fi

# 8c-3: pool scaled down during idle period
if [ "$DOWN_AFTER_IDLE" -ge 1 ]; then
    echo "PASS: idle timeout triggered scale-down ($DOWN_AFTER_IDLE events)"
else
    echo "FAIL: no scale-down during idle period"
    exit 1
fi

# 8c-4: worker count dropped toward core after idle
if [ "$LAST_COUNT_AFTER_IDLE" -le 4 ]; then
    echo "PASS: worker count reduced after idle ($LAST_COUNT_AFTER_IDLE <= 4)"
else
    echo "FAIL: worker count $LAST_COUNT_AFTER_IDLE still high after idle"
    exit 1
fi

# 8c-5: pool re-scaled up for 2nd batch (more scale-up events than after 1st)
if [ "$UP_AFTER_2ND" -gt "$UP_AFTER_1ST" ]; then
    echo "PASS: 2nd batch triggered re-scale ($UP_AFTER_2ND > $UP_AFTER_1ST)"
else
    echo "FAIL: no re-scale after 2nd batch ($UP_AFTER_2ND == $UP_AFTER_1ST)"
    exit 1
fi

echo ""
echo "=== Pool lifecycle (re-scale test) ==="
grep -E "(created|scaled up|idle timeout|all workers|peak)" logs/server.log 2>/dev/null || true

# ================================================================
# Cleanup
# ================================================================
rm -f logs/server.log
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY08 PASS"
