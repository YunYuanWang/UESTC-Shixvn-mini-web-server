#!/usr/bin/env bash
set -e

# ================================================================
# test_day15.sh — v1.5 Route Table & Configuration Tests
#
# Tests:
#   1. Exact path matching
#   2. Prefix/parameterized path matching (trailing wildcard)
#   3. Exact path priority over prefix path
#   4. 405 + Allow header when path exists but method mismatches
#   5. 404 when no route matches
#   6. Duplicate rule detection → server refuses to start
#   7. Unknown handler detection → server refuses to start
#   8. Log level from config
# ================================================================

HOST="127.0.0.1"
PORT="9280"
PASS=0
FAIL=0
TOTAL=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}PASS${NC}: $1"; PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); }
fail() { echo -e "  ${RED}FAIL${NC}: $1"; FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); }
info() { echo -e "  ${YELLOW}INFO${NC}: $1"; }

# Helper: send raw HTTP via nc and capture response
nc_send() {
    printf '%b' "$1" | nc -w 2 ${HOST} ${PORT} 2>/dev/null || true
}

# Helper: get status line
nc_status() {
    nc_send "$1" | head -1 | tr -d '\r'
}

# Helper: check status code
check_status() {
    local expected="$1"
    local desc="$2"
    local request="$3"
    local resp
    resp=$(nc_status "$request")
    if echo "$resp" | grep -qF "$expected"; then
        pass "$desc — got '$resp'"
    else
        fail "$desc — expected '$expected', got '$resp'"
    fi
}

# Helper: check response header contains a value
check_header() {
    local header="$1"
    local desc="$2"
    local request="$3"
    local resp
    resp=$(nc_send "$request")
    if echo "$resp" | grep -qi "$header"; then
        pass "$desc"
    else
        fail "$desc — header/body missing '$header'"
    fi
}

# Helper: check server exits with non-zero code
check_startup_fail() {
    local config_content="$1"
    local desc="$2"
    local tmpconf="/tmp/d15_fail_test.conf"

    echo "$config_content" > "$tmpconf"
    set +e
    ./mini_web_server master "$tmpconf" > /tmp/d15_startup.log 2>&1
    local exit_code=$?
    set -e
    rm -f "$tmpconf"

    if [ "$exit_code" -ne 0 ]; then
        pass "$desc — server refused to start (exit=$exit_code)"
    else
        fail "$desc — server started but should have rejected config"
        # Kill any orphan server
        fuser -k ${PORT}/tcp 2>/dev/null || true
    fi
}

# ================================================================
# Setup
# ================================================================
echo "=============================================="
echo " test_day15.sh — v1.5 Route Table & Config Tests"
echo "=============================================="
echo ""

# Free the port
fuser -k ${PORT}/tcp 2>/dev/null || true
sleep 0.3

# Build if needed
if [ ! -x "./mini_web_server" ]; then
    make clean > /dev/null 2>&1
    make > /dev/null 2>&1
fi

# ================================================================
# Part A: Startup validation tests (before starting server)
# ================================================================
echo "--- Part A: Startup Validation ---"
echo ""

# Test A1: Duplicate route detection
info "Test A1: Duplicate routes should refuse startup"
check_startup_fail \
"worker_processes 1
worker_shutdown_timeout_ms 2000
max_request_bytes 4096
user_file data/users_100000_c.csv
log_level none
system_log logs/system.log
access_log logs/access.log
server {
    listen 127.0.0.1:${PORT} default_server
    server_name test.local
    root search
}
location = /hello {
    handler hello;
    methods GET;
}
location = /hello {
    handler hello;
    methods GET;
}" \
    "Duplicate exact route '/hello'"

# Test A2: Unknown handler detection
info "Test A2: Unknown handler should refuse startup"
check_startup_fail \
"worker_processes 1
worker_shutdown_timeout_ms 2000
max_request_bytes 4096
user_file data/users_100000_c.csv
log_level none
system_log logs/system.log
access_log logs/access.log
server {
    listen 127.0.0.1:${PORT} default_server
    server_name test.local
    root search
}
location = /test {
    handler nonexistent_handler;
    methods GET;
}" \
    "Unknown handler 'nonexistent_handler'"

# Test A3: Empty methods list
info "Test A3: Empty methods should refuse startup"
check_startup_fail \
"worker_processes 1
worker_shutdown_timeout_ms 2000
max_request_bytes 4096
user_file data/users_100000_c.csv
log_level none
system_log logs/system.log
access_log logs/access.log
server {
    listen 127.0.0.1:${PORT} default_server
    server_name test.local
    root search
}
location = /test {
    handler hello;
}" \
    "Empty methods list"

# ================================================================
# Part B: Start server with route-table config
# ================================================================
echo ""
echo "--- Part B: Start Server with Route Config ---"

cat > /tmp/d15_server.conf << CONFEOF
worker_processes 1
worker_shutdown_timeout_ms 2000
max_connections 256
max_request_bytes 4096
user_file data/users_100000_c.csv
log_level info
system_log logs/system.log
access_log logs/access.log

server {
    listen 127.0.0.1:${PORT} default_server
    server_name test.local
    root search
}

# ---- v1.5 Route Definitions (one entry per method+path) ----

# Exact routes
location = /hello {
    handler hello;
    methods GET;
}

location = /users {
    handler user_list;
    methods GET;
}

location = /users {
    handler user_add;
    methods POST;
}

location = /help {
    handler help;
    methods GET;
}

location = /search {
    handler search;
    methods POST;
}

location = /delete {
    handler delete_form;
    methods POST;
}

# Prefix/wildcard routes (longer prefixes first)
location /users/find-index/* {
    handler user_find_index;
    methods GET;
}

location /users/compare-verbose/* {
    handler user_compare_verbose;
    methods GET;
}

location /users/compare/* {
    handler user_compare;
    methods GET;
}

location /users/* {
    handler user_by_name;
    methods GET;
}

location /users/* {
    handler user_delete;
    methods DELETE;
}

location /user/* {
    handler user_simple_find;
    methods GET;
}

location /sleep/* {
    handler sleep;
    methods GET;
}

# Blog routes
location = /blog {
    handler blog;
    methods GET HEAD;
}

location /blog/* {
    handler blog;
    methods GET HEAD;
}

# Index + static file fallback
location = / {
    handler index;
    methods GET HEAD;
}

location /* {
    handler static;
    methods GET HEAD;
}
CONFEOF

./mini_web_server master /tmp/d15_server.conf > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FATAL: Server failed to start"
    exit 1
fi
info "Server PID=$SERVER_PID started on port $PORT (route-table config)"

# ================================================================
# Part C: Route matching tests
# ================================================================

# --- Test C1: Exact path matching ---
echo ""
echo "--- Test C1: Exact Path Matching ---"
check_status "HTTP/1.1 200" \
    "GET /hello → 200 (exact match)" \
    "GET /hello HTTP/1.1\r\nHost: test.local\r\n\r\n"

check_status "HTTP/1.1 200" \
    "GET /help → 200 (exact match)" \
    "GET /help HTTP/1.1\r\nHost: test.local\r\n\r\n"

check_status "HTTP/1.1 200" \
    "GET /users → 200 (exact match)" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

# --- Test C2: Prefix/parameterized path matching ---
echo ""
echo "--- Test C2: Prefix/Parameterized Path Matching ---"

# /sleep/100 → captured="100", always returns 200
check_status "HTTP/1.1 200" \
    "GET /sleep/100 → 200 (prefix match, captured='100')" \
    "GET /sleep/100 HTTP/1.1\r\nHost: test.local\r\n\r\n"

# /user/alice → captured="alice", user doesn't exist → handler returns 404
# This confirms routing worked (prefix match + capture), even though lookup fails
check_status "HTTP/1.1 404" \
    "GET /user/alice → 404 (prefix match OK, user not found)" \
    "GET /user/alice HTTP/1.1\r\nHost: test.local\r\n\r\n"

check_status "HTTP/1.1 200" \
    "GET /blog/ → 200 (prefix, captured='')" \
    "GET /blog/ HTTP/1.1\r\nHost: test.local\r\n\r\n"

# --- Test C3: Exact priority over prefix ---
echo ""
echo "--- Test C3: Exact Path Priority Over Prefix ---"
# /users is exact (HANDLER_USERS → GET=user_list), /users/* is prefix (HANDLER_USER_RESOURCE → GET=user_by_name)
# GET /users should match exact route (user_list), not prefix route (user_by_name for name="")
check_status "HTTP/1.1 200" \
    "GET /users → exact match (list), not prefix (find '')" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

# --- Test C4: 405 + Allow header ---
echo ""
echo "--- Test C4: 405 Method Not Allowed + Allow Header ---"
check_status "HTTP/1.1 405" \
    "POST /hello → 405 (path exists, method not in GET)" \
    "POST /hello HTTP/1.1\r\nHost: test.local\r\nContent-Length: 0\r\n\r\n"

check_header "Allow:" \
    "/hello POST response has Allow: GET header" \
    "POST /hello HTTP/1.1\r\nHost: test.local\r\nContent-Length: 0\r\n\r\n"

check_status "HTTP/1.1 405" \
    "DELETE /users → 405 (path exists, method not in GET,POST)" \
    "DELETE /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

check_header "Allow:" \
    "/users DELETE response has Allow header" \
    "DELETE /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

# --- Test C5: 404 Not Found ---
echo ""
echo "--- Test C5: 404 Not Found ---"
check_status "HTTP/1.1 404" \
    "GET /nonexistent/path → 404" \
    "GET /nonexistent/path HTTP/1.1\r\nHost: test.local\r\n\r\n"

check_status "HTTP/1.1 404" \
    "GET /also/not/real → 404" \
    "GET /also/not/real HTTP/1.1\r\nHost: test.local\r\n\r\n"

# --- Test C6: POST /users through route table ---
echo ""
echo "--- Test C6: POST /users (method-aware handler) ---"
check_status "HTTP/1.1 200" \
    "POST /users → routed via 'users' handler" \
    "POST /users HTTP/1.1\r\nHost: test.local\r\nContent-Length: 69\r\n\r\ntestroute15,pass123,1990-01-01,010-1234,13800138000,test@test.com"

# --- Test C7: DELETE /users/<name> ---
echo ""
echo "--- Test C7: DELETE /users/<name> (method-aware handler) ---"
check_status "HTTP/1.1 200" \
    "DELETE /users/testroute15 → 200 (deleted)" \
    "DELETE /users/testroute15 HTTP/1.1\r\nHost: test.local\r\n\r\n"

# --- Test C8: GET /users/find-index/ ---
echo ""
echo "--- Test C8: Long prefix before short prefix ---"
# /users/find-index/* should match before /users/*
check_status "HTTP/1.1 404" \
    "GET /users/find-index/zzz → 404 (not found)" \
    "GET /users/find-index/zzz HTTP/1.1\r\nHost: test.local\r\n\r\n"

# ================================================================
# Cleanup
# ================================================================
echo ""
echo "=============================================="
echo " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${TOTAL} total"
echo "=============================================="

# Stop server
if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -INT "$SERVER_PID" 2>/dev/null || true
    sleep 1
    kill -9 "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
fi
info "Server stopped"

# Cleanup temp configs
rm -f /tmp/d15_server.conf /tmp/d15_fail_test.conf

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo -e "${RED}Some tests FAILED. Check output above.${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}All tests PASSED.${NC}"
    exit 0
fi
