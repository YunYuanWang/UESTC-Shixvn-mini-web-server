#!/usr/bin/env bash
set -e

# ================================================================
# test_day16.sh — v1.6 HTTP Basic Auth (Role-Based)
#
# Tests:
#   1. Public URL → 200 (no auth)
#   2. Admin route, no credentials → 401 + WWW-Authenticate
#   3. Admin route + admin credentials → 200
#   4. Admin route + user credentials (wrong role) → 403
#   5. Admin route + invalid password → 401
#   6. User route + user credentials → 200
#   7. User route + admin credentials (admin is super-role) → 200
#   8. Malformed Base64 → 400
# ================================================================

HOST="127.0.0.1"
PORT="9380"
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

nc_send() {
    printf '%b' "$1" | nc -w 2 ${HOST} ${PORT} 2>/dev/null || true
}

nc_status() {
    nc_send "$1" | head -1 | tr -d '\r'
}

check_status() {
    local expected="$1" desc="$2" request="$3"
    local resp=$(nc_status "$request")
    if echo "$resp" | grep -qF "$expected"; then
        pass "$desc — $resp"
    else
        fail "$desc — expected '$expected', got '$resp'"
    fi
}

check_header() {
    local header="$1" desc="$2" request="$3"
    local resp=$(nc_send "$request")
    if echo "$resp" | grep -qi "$header"; then
        pass "$desc"
    else
        fail "$desc — missing '$header'"
    fi
}

# ================================================================
echo "=============================================="
echo " test_day16.sh — v1.6 Basic Auth (Role-Based)"
echo "=============================================="
echo ""

fuser -k ${PORT}/tcp 2>/dev/null || true
sleep 0.3

if [ ! -x "./mini_web_server" ]; then
    make clean > /dev/null 2>&1
    make > /dev/null 2>&1
fi

# ---- Start server with auth config ----
cat > /tmp/d16_server.conf << 'CONFEOF'
worker_processes 1
worker_shutdown_timeout_ms 2000
max_request_bytes 4096
user_file data/users_100000_c.csv
auth_user_file conf/.htpasswd
log_level warning
system_log logs/system.log
access_log logs/access.log

server {
    listen 127.0.0.1:9380 default_server
    server_name test.local
    root search
}

# Public routes
location = /hello {
    handler hello;
    methods GET;
}

# Admin routes
location = /users {
    auth_basic "Admin Area";
    auth_role admin;
    handler user_list;
    methods GET;
}

# User routes
location = /search {
    auth_basic "User Area";
    auth_role user;
    handler search;
    methods POST;
}
CONFEOF

./mini_web_server master /tmp/d16_server.conf > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FATAL: Server failed to start"
    exit 1
fi
info "Server PID=$SERVER_PID on port $PORT"

# ---- Test 1: Public URL, no auth → 200 ----
echo ""
echo "--- Test 1: Public URL (no auth) ---"
check_status "HTTP/1.1 200" \
    "GET /hello → 200" \
    "GET /hello HTTP/1.1\r\nHost: test.local\r\n\r\n"

# ---- Test 2: Admin route, no credentials → 401 ----
echo ""
echo "--- Test 2: Admin route, no auth → 401 ---"
check_status "HTTP/1.1 401" \
    "GET /users, no credentials → 401" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

check_header "WWW-Authenticate:" \
    "401 response has WWW-Authenticate header" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

# ---- Test 3: Admin route + admin credentials → 200 ----
# admin:secret123 = YWRtaW46c2VjcmV0MTIz
echo ""
echo "--- Test 3: Admin route + admin credentials → 200 ---"
check_status "HTTP/1.1 200" \
    "GET /users with admin:secret123 → 200" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nAuthorization: Basic YWRtaW46c2VjcmV0MTIz\r\n\r\n"

# ---- Test 4: Admin route + user credentials (wrong role) → 403 ----
# user:password = dXNlcjpwYXNzd29yZA==
echo ""
echo "--- Test 4: Admin route + user credentials → 403 ---"
check_status "HTTP/1.1 403" \
    "GET /users with user:password (role=user) → 403" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nAuthorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\n"

# ---- Test 5: Admin route + invalid password → 401 ----
# admin:wrong = YWRtaW46d3Jvbmc=
echo ""
echo "--- Test 5: Admin route + wrong password → 401 ---"
check_status "HTTP/1.1 401" \
    "GET /users with admin:wrong → 401" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nAuthorization: Basic YWRtaW46d3Jvbmc=\r\n\r\n"

# ---- Test 6: User route + user credentials → 200 ----
echo ""
echo "--- Test 6: User route + user credentials → 200 ---"
check_status "HTTP/1.1 200" \
    "POST /search with user:password → 200" \
    "POST /search HTTP/1.1\r\nHost: test.local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 5\r\nAuthorization: Basic dXNlcjpwYXNzd29yZA==\r\n\r\nname="

# ---- Test 7: User route + admin credentials → 200 (admin is super-role) ----
echo ""
echo "--- Test 7: Admin on user route (super-role) → 200 ---"
check_status "HTTP/1.1 200" \
    "POST /search with admin:secret123 → 200" \
    "POST /search HTTP/1.1\r\nHost: test.local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 5\r\nAuthorization: Basic YWRtaW46c2VjcmV0MTIz\r\n\r\nname="

# ---- Test 8: Malformed Base64 → 400 ----
echo ""
echo "--- Test 8: Malformed Base64 → 400 ---"
check_status "HTTP/1.1 400" \
    "GET /users with bad Base64 → 400" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nAuthorization: Basic !!!bad!!!\r\n\r\n"

# ---- Cleanup ----
echo ""
echo "=============================================="
echo " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${TOTAL} total"
echo "=============================================="

if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -INT "$SERVER_PID" 2>/dev/null || true
    sleep 1
    kill -9 "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
fi
rm -f /tmp/d16_server.conf

if [ "$FAIL" -gt 0 ]; then
    echo -e "${RED}Some tests FAILED.${NC}"
    exit 1
else
    echo -e "${GREEN}All tests PASSED.${NC}"
    exit 0
fi
