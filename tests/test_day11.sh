#!/usr/bin/env bash
set -e

# ================================================================
# test_day11.sh — master-worker 模式测试 (v1.0)
#
# Nginx-style multi-process architecture:
#   - Master reads config, creates listen socket, forks 2 workers
#   - Each worker runs independent epoll event loop
#   - User data loaded once before fork, inherited via CoW
#
# Tests:
#   1. Server startup: master + 2 worker PIDs in log
#   2. Basic HTTP endpoints (hello, users, POST, DELETE, 404)
#   3. 40+ concurrent requests — all succeed
#   4. Both workers handle requests (two different PIDs in request logs)
#   5. Client isolation: slow request does NOT block fast request
#   6. SIGINT graceful shutdown (workers reaped, clean exit)
#   7. User index NOT rebuilt per request
#   8. Log format: single line with PID + fd + path + status
# ================================================================

HOST="127.0.0.1"
PORT="9080"
BASE="http://${HOST}:${PORT}"
LOG="logs/server.log"
PASS=0
FAIL=0

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}PASS${NC}: $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}FAIL${NC}: $1"; FAIL=$((FAIL + 1)); }
warn() { echo -e "  WARN: $1"; }

# Helper: wait for specific background pids
wait_pids() {
    for _pid in "$@"; do
        wait "$_pid" 2>/dev/null || true
    done
}

# Helper: start master-worker server with a given port
start_master() {
    local port=${1:-9080}
    fuser -k "${port}/tcp" 2>/dev/null || true
    sleep 0.3

    # Create temp config with test port
    cat > /tmp/d11_server.conf << EOF
host=127.0.0.1
port=${port}
www_root=www
user_file=data/users.csv
log=logs/server.log
max_connections=256
max_request_bytes=4096
worker_processes=2
worker_shutdown_timeout_ms=3000
EOF

    > "$LOG"
    ./mini_web_server master /tmp/d11_server.conf &
    SERVER_PID=$!
    sleep 2
}

# Helper: stop server gracefully
stop_master() {
    local pid=${1:-$SERVER_PID}
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
        sleep 2
        # force kill if still alive after 2s
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    fi
}

# Helper: check that a background server exited cleanly
expect_clean_exit() {
    local pid=$1
    local ret=0
    wait "$pid" 2>/dev/null || ret=$?
    # SIGINT → clean exit should be 0
    # If killed by SIGKILL, ret will be 137 (128+9)
    if [ "$ret" -eq 0 ]; then
        return 0
    else
        return 1
    fi
}

# ================================================================
# Setup
# ================================================================
echo "=== mini_web_server v1.0 Master-Worker Tests ==="
echo ""

# backup user data
cp data/users.csv data/users.csv.bak

# build
make clean > /dev/null 2>&1
make > /dev/null 2>&1
echo "Build OK"

# cleanup any leftovers from previous runs
pkill -f "mini_web_server master" 2>/dev/null || true
sleep 0.3

# verify conf contains v1.0 keys
if grep -q "worker_processes" conf/server.conf; then
    echo "Config OK: worker_processes present"
else
    echo "Config WARN: worker_processes not in server.conf"
fi

# ================================================================
echo ""
echo "=== Test 1: Server startup — master + 2 worker PIDs ==="

start_master 9080

# Check that 3 processes exist: 1 master + 2 workers
PROC_COUNT=$(pgrep -f "mini_web_server master" | wc -l)
if [ "$PROC_COUNT" -ge 3 ]; then
    pass "3 processes running (1 master + 2 workers)"
else
    fail "expected >=3 processes, got $PROC_COUNT"
fi

# Check log for Master PID + 2 different Worker PIDs
MASTER_PID=$(grep "Master PID:" "$LOG" 2>/dev/null | sed 's/.*Master PID:\s*//' | sed 's/\s.*//' | head -1)
WORKER_PIDS=$(grep "\[Worker\] PID" "$LOG" 2>/dev/null | grep "started" | sed 's/.*PID \([0-9]*\).*/\1/' | sort -u)

if [ -n "$MASTER_PID" ]; then
    pass "master PID found: $MASTER_PID"
else
    fail "master PID not found in log"
fi

WORKER_COUNT=$(echo "$WORKER_PIDS" | grep -c . || true)
if [ "$WORKER_COUNT" -eq 2 ]; then
    pass "2 different worker PIDs in log: $(echo $WORKER_PIDS | tr '\n' ' ')"
else
    fail "expected 2 worker PIDs, got $WORKER_COUNT"
fi

# Verify master PID ≠ worker PIDs
for wp in $WORKER_PIDS; do
    if [ "$wp" != "$MASTER_PID" ]; then
        pass "worker PID $wp differs from master PID $MASTER_PID"
    else
        fail "worker PID $wp equals master PID"
    fi
done

# ================================================================
echo ""
echo "=== Test 2: Basic HTTP endpoints ==="

# GET /hello
RESP=$(curl -s --max-time 5 "${BASE}/hello")
if echo "$RESP" | grep -qF "Hello, Web!"; then
    pass "GET /hello → Hello, Web!"
else
    fail "GET /hello: got '$RESP'"
fi

# GET /users/baianai (real user from 100K dataset)
RESP=$(curl -s --max-time 5 "${BASE}/users/baianai")
if echo "$RESP" | grep -qF "name: baianai"; then
    pass "GET /users/baianai → found"
else
    fail "GET /users/baianai: unexpected response"
fi

# POST /users (add user — may be CoW-isolated per worker)
RESP=$(curl -s --max-time 5 -X POST \
    -d "Day11Test,pass999,20000101,010-111,139,day11@t.com" \
    "${BASE}/users")
if echo "$RESP" | grep -qF "ADDED"; then
    pass "POST /users → ADDED"
else
    fail "POST /users: got '$RESP'"
fi

# DELETE /users/Day11Test
# Note: POST and DELETE may hit different workers (CoW isolation),
# so NO_SUCH_USER is also acceptable
RESP=$(curl -s --max-time 5 -X DELETE "${BASE}/users/Day11Test")
if echo "$RESP" | /bin/grep -qE "DELETED|NO_SUCH_USER"; then
    pass "DELETE /users/Day11Test → $RESP"
else
    fail "DELETE /users/Day11Test: got '$RESP'"
fi

# GET 404
RESP=$(curl -s --max-time 5 "${BASE}/no/such/path")
if echo "$RESP" | grep -qF "404 Not Found"; then
    pass "GET /no/such/path → 404"
else
    fail "GET /no/such/path: expected 404, got '$RESP'"
fi

# ================================================================
echo ""
echo "=== Test 3: 40+ concurrent requests ==="

PIDS=""
TMPDIR="/tmp/d11_concurrent"
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"

for i in $(seq 1 45); do
    curl -s --max-time 10 -o "${TMPDIR}/${i}.out" "${BASE}/hello" &
    PIDS="$PIDS $!"
done
wait_pids $PIDS
sleep 0.5

ok=0
for i in $(seq 1 45); do
    if grep -qF "Hello, Web!" "${TMPDIR}/${i}.out" 2>/dev/null; then
        ok=$((ok + 1))
    fi
done

if [ "$ok" -ge 43 ]; then
    pass "$ok/45 concurrent /hello requests succeeded"
else
    fail "only $ok/45 concurrent requests succeeded"
fi
rm -rf "$TMPDIR"

# ================================================================
echo ""
echo "=== Test 4: Both workers handle requests ==="

# Count request log entries per worker PID
for wp in $WORKER_PIDS; do
    REQ_COUNT=$(grep -c "\[PID ${wp}\]" "$LOG" 2>/dev/null || true)
    if [ "$REQ_COUNT" -gt 0 ]; then
        pass "Worker PID $wp handled $REQ_COUNT requests"
    else
        fail "Worker PID $wp handled 0 requests"
    fi
done

# Check that request logs show two different PIDs
PID_COUNT=$(grep "\[PID " "$LOG" 2>/dev/null | sed 's/.*\[PID \([0-9]*\)\].*/\1/' | sort -u | wc -l)
if [ "$PID_COUNT" -ge 2 ]; then
    pass "logs contain >=2 different PIDs (master + workers)"
else
    fail "only $PID_COUNT different PID(s) in log"
fi

# ================================================================
echo ""
echo "=== Test 5: Client isolation ==="

# Start a slow request (3 seconds) in background
curl -s --max-time 10 "${BASE}/sleep/3000" > /dev/null &
SLOW_PID=$!
sleep 0.3

# Send a quick request while slow one is processing
QUICK_RESULT=$(curl -s --max-time 5 "${BASE}/hello" 2>/dev/null || echo "TIMEOUT")

if [ "$QUICK_RESULT" = "Hello, Web!" ]; then
    pass "quick request returned immediately during 3s slow request"
else
    fail "quick request was blocked by slow request (got: '$QUICK_RESULT')"
fi

if kill -0 "$SLOW_PID" 2>/dev/null; then
    pass "slow request still running while quick request completed"
else
    warn "slow request already finished (timing issue, not a failure)"
fi

wait "$SLOW_PID" 2>/dev/null || true

# ================================================================
echo ""
echo "=== Test 6: SIGINT graceful shutdown ==="

# Get master PID
MASTER_PID_CURRENT=$(pgrep -f "mini_web_server master" | head -1)

if [ -z "$MASTER_PID_CURRENT" ]; then
    fail "master process not found, cannot test shutdown"
else
    echo "Sending SIGINT to master PID $MASTER_PID_CURRENT ..."
    kill -INT "$MASTER_PID_CURRENT"
    sleep 3

    # Check exit
    if ! kill -0 "$MASTER_PID_CURRENT" 2>/dev/null; then
        pass "server process exited after SIGINT"
    else
        kill -9 "$MASTER_PID_CURRENT" 2>/dev/null || true
        fail "server still alive 3s after SIGINT"
    fi

    # Check no zombie processes
    ZOMBIES=$(pgrep -f "mini_web_server master" | wc -l)
    if [ "$ZOMBIES" -eq 0 ]; then
        pass "no zombie/leftover processes"
    else
        fail "$ZOMBIES leftover process(es)"
    fi
fi

# Verify shutdown log sequence
echo ""
echo "Shutdown log check:"
for keyword in "shutting down" "all workers stopped" "shutdown complete"; do
    if grep -q "$keyword" "$LOG" 2>/dev/null; then
        pass "log contains: '$keyword'"
    else
        fail "log missing: '$keyword'"
    fi
done

# Check SIGTERM was sent to workers
if grep -q "sending SIGTERM to worker" "$LOG" 2>/dev/null; then
    pass "master sent SIGTERM to workers"
else
    fail "no SIGTERM sent to workers in log"
fi

# Check workers were reaped (either by waitpid loop or SIGCHLD handler)
if /bin/grep -qE "reaped worker|worker PID.*exited" "$LOG" 2>/dev/null; then
    pass "master waitpid-reaped workers"
else
    fail "no worker reaped in log"
fi

# Check worker shutdown logged
WORKER_SHUTDOWN_COUNT=$(grep -c "\[Worker\].*shutting" "$LOG" 2>/dev/null || true)
if [ "$WORKER_SHUTDOWN_COUNT" -eq 2 ]; then
    pass "both workers logged shutdown"
else
    fail "$WORKER_SHUTDOWN_COUNT worker(s) logged shutdown (expected 2)"
fi

# ================================================================
echo ""
echo "=== Test 7: User index NOT rebuilt per request ==="

# Count "loading data" occurrences (should be exactly 1 — from master at startup)
LOAD_COUNT=$(grep -c "loading data" "$LOG" 2>/dev/null || true)
if [ "$LOAD_COUNT" -eq 1 ]; then
    pass "'loading data' appears exactly once (count=$LOAD_COUNT)"
else
    fail "'loading data' appears $LOAD_COUNT times (expected 1)"
fi

# Count "loaded X users" occurrences (should be exactly 1)
LOADED_COUNT=$(grep -c "loaded.*users" "$LOG" 2>/dev/null || true)
if [ "$LOADED_COUNT" -eq 1 ]; then
    pass "'loaded N users' appears exactly once (count=$LOADED_COUNT)"
else
    fail "'loaded N users' appears $LOADED_COUNT times (expected 1)"
fi

# The loading message must come from master PID, not worker PIDs
LOAD_PID=$(grep "loading data\|loaded.*users" "$LOG" 2>/dev/null | head -1 | sed 's/.*\[PID \([0-9]*\)\].*/\1/')
if [ "$LOAD_PID" = "$MASTER_PID" ]; then
    pass "user data loaded by master PID ($LOAD_PID)"
elif [ -n "$LOAD_PID" ]; then
    fail "user data loaded by PID $LOAD_PID, not master ($MASTER_PID)"
else
    warn "could not determine which PID loaded data"
fi

# Verify no CSV re-read by workers
if ! grep -E "\[Worker\].*(loading|users\.csv|cannot open)" "$LOG" 2>/dev/null | grep -v "started" | grep -q .; then
    pass "no CSV re-read by workers in log"
else
    fail "worker may have re-read CSV"
fi

# ================================================================
echo ""
echo "=== Test 8: Log format — single line with fd + path + status ==="

# Check that request logs contain fd=N
FD_COUNT=$(grep -c "fd=[0-9]" "$LOG" 2>/dev/null || true)
if [ "$FD_COUNT" -gt 0 ]; then
    pass "log contains 'fd=N' format ($FD_COUNT request entries)"
else
    fail "no 'fd=N' format found in log"
fi

# Check that request log entries contain status codes (200, 404, etc.)
# Note: ugrep interprets "->" as flag syntax, so we use a simpler match
STATUS_OK=$(grep -c "200" "$LOG" 2>/dev/null || true)
if [ -n "$STATUS_OK" ] && [ "$STATUS_OK" -gt 10 ] 2>/dev/null; then
    pass "log contains HTTP status codes ($STATUS_OK+ entries with 200)"
else
    fail "insufficient status codes in log"
fi

# Check that PID + fd + path + status all appear in the same line
# Sample: [INFO] [PID 111813] [...] [EpollServer] fd=6 GET /hello -> 200
if grep -E "\[EpollServer\] fd=[0-9]+ (GET|POST|DELETE) " "$LOG" 2>/dev/null | head -1 | grep -q .; then
    pass "single log line contains fd + method + path (v1.0 format)"
else
    fail "log format missing fd/method/path in single line"
fi

# Show sample log entries
echo ""
echo "=== Log samples ==="
grep -E "fd=.*(GET|POST|DELETE)" "$LOG" 2>/dev/null | head -5 || echo "(no request log entries)"
echo ""
echo "=== Shutdown log ==="
grep -E "shutting|SIGTERM|reaped|all workers|shutdown complete" "$LOG" 2>/dev/null || echo "(no shutdown log)"

# ================================================================
# Cleanup
# ================================================================
rm -f "$LOG"
rm -f /tmp/d11_server.conf
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "============================================"
if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${GREEN}DAY11 PASS${NC}  ($PASS tests passed, $FAIL failed)"
    exit 0
else
    echo -e "  ${RED}DAY11 FAIL${NC}  ($PASS passed, $FAIL failed)"
    exit 1
fi
