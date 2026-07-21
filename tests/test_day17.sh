#!/usr/bin/env bash
set -e

# ================================================================
# test_day17.sh — v1.7 Session/Cookie Authentication
# ================================================================

HOST="127.0.0.1"
PORT="9480"
PASS=0; FAIL=0; TOTAL=0
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}PASS${NC}: $1"; PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); }
fail() { echo -e "  ${RED}FAIL${NC}: $1"; FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); }
info() { echo -e "  ${YELLOW}INFO${NC}: $1"; }

nc_send() { printf '%b' "$1" | nc -w 2 ${HOST} ${PORT} 2>/dev/null || true; }
nc_status() { nc_send "$1" | head -1 | tr -d '\r'; }
check_status() {
    local expected="$1" desc="$2" request="$3"
    local resp=$(nc_status "$request")
    if echo "$resp" | grep -qF "$expected"; then pass "$desc — $resp"
    else fail "$desc — expected '$expected', got '$resp'"; fi
}
check_header() {
    local header="$1" desc="$2" request="$3"
    local resp=$(nc_send "$request")
    if echo "$resp" | grep -qi "$header"; then pass "$desc"
    else fail "$desc — missing '$header'"; fi
}
extract_cookie() {
    printf '%b' "$1" | nc -w 2 ${HOST} ${PORT} 2>/dev/null | grep -i "^Set-Cookie:" | head -1 | tr -d '\r'
}

# ================================================================
echo "=============================================="
echo " test_day17.sh — v1.7 Session/Cookie Auth"
echo "=============================================="
echo ""

fuser -k ${PORT}/tcp 2>/dev/null || true; sleep 0.3

if [ ! -x "./mini_web_server" ]; then
    make clean > /dev/null 2>&1; make > /dev/null 2>&1
fi

# ---- Config with login + admin routes ----
cat > /tmp/d17_server.conf << 'CONFEOF'
worker_processes 1
worker_shutdown_timeout_ms 2000
max_request_bytes 4096
user_file data/users_100000_c.csv
auth_user_file conf/.htpasswd
log_level warning
system_log logs/system.log
access_log logs/access.log
www_root search
server {
    listen 127.0.0.1:9480 default_server
    root search
}

# Public
location = /hello {
    handler hello;
    methods GET;
}

# Login/logout (public)
location = /login {
    handler login;
    methods GET POST;
}
location = /logout {
    handler logout;
    methods POST;
}

# Admin routes (session or Basic auth)
location = /users {
    auth_basic "Admin Area"
    auth_role admin
    handler user_list
    methods GET;
}

# User routes
location = /search {
    auth_basic "User Area"
    auth_role user
    handler search
    methods POST;
}
CONFEOF

./mini_web_server master /tmp/d17_server.conf > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FATAL: Server failed to start"
    exit 1
fi
info "Server PID=$SERVER_PID on port $PORT"

# ---- Test 1: Public URL → 200 ----
echo ""; echo "--- Test 1: Public URL ---"
check_status "HTTP/1.1 200" "GET /hello" \
    "GET /hello HTTP/1.1\r\nHost: test.local\r\n\r\n"

# ---- Test 2: Protected URL, no auth → 401 ----
echo ""; echo "--- Test 2: No auth → 302 redirect ---"
check_status "HTTP/1.1 302" "GET /users (no auth)" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\n\r\n"

# ---- Test 3: Login with valid credentials → 302 + Set-Cookie ----
echo ""; echo "--- Test 3: Login → 302 + Set-Cookie ---"
LOGIN_REQ="POST /login HTTP/1.1\r\nHost: test.local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 30\r\n\r\nusername=admin&password=secret123"
check_status "HTTP/1.1 302" "POST /login admin:secret123" "$LOGIN_REQ"
check_header "Set-Cookie:" "Login sets session cookie" "$LOGIN_REQ"

# Extract session cookie
SID=$(extract_cookie "$LOGIN_REQ" | sed 's/.*session_id=//;s/;.*//')
info "Session ID: $SID"

# ---- Test 4: Login with invalid credentials → 401 ----
echo ""; echo "--- Test 4: Bad login → 401 ---"
BAD_LOGIN="POST /login HTTP/1.1\r\nHost: test.local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 26\r\n\r\nusername=admin&password=wrong"
check_status "HTTP/1.1 401" "POST /login bad password" "$BAD_LOGIN"

# ---- Test 5: Protected URL with valid cookie → 200 ----
echo ""; echo "--- Test 5: Cookie auth → 200 ---"
check_status "HTTP/1.1 200" "GET /users with session cookie" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nCookie: session_id=$SID\r\n\r\n"

# ---- Test 6: Protected URL, cookie valid but role mismatch → 403 ----
echo ""; echo "--- Test 6: Cookie role mismatch (user cookie on admin route) ---"
# Login as user
USER_LOGIN="POST /login HTTP/1.1\r\nHost: test.local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 28\r\n\r\nusername=user&password=password"
USER_SID=$(extract_cookie "$USER_LOGIN" | sed 's/.*session_id=//;s/;.*//')
info "User session: $USER_SID"
check_status "HTTP/1.1 403" "GET /users with user cookie" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nCookie: session_id=$USER_SID\r\n\r\n"

# ---- Test 7: Protected URL with fake cookie → 302 to /login ----
echo ""; echo "--- Test 7: Fake cookie → 302 (redirect to login) ---"
check_status "HTTP/1.1 302" "GET /users with fake cookie" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nCookie: session_id=fakeinvalid123\r\n\r\n"

# ---- Test 8: Logout → 302 + clear cookie ----
echo ""; echo "--- Test 8: Logout → 302 + expired cookie ---"
LOGOUT_REQ="POST /logout HTTP/1.1\r\nHost: test.local\r\nCookie: session_id=$SID\r\nContent-Length: 0\r\n\r\n"
check_status "HTTP/1.1 302" "POST /logout" "$LOGOUT_REQ"
check_header "Set-Cookie:" "Logout clears cookie" "$LOGOUT_REQ"

# ---- Test 9: After logout, cookie no longer works → 302 to /login ----
echo ""; echo "--- Test 9: After logout → 302 (redirect to login) ---"
check_status "HTTP/1.1 302" "GET /users with old cookie after logout" \
    "GET /users HTTP/1.1\r\nHost: test.local\r\nCookie: session_id=$SID\r\n\r\n"

# ---- Cleanup ----
echo ""; echo "=============================================="
echo " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}, ${TOTAL} total"
echo "=============================================="

if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -INT "$SERVER_PID" 2>/dev/null || true; sleep 1
    kill -9 "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true
fi
rm -f /tmp/d17_server.conf

if [ "$FAIL" -gt 0 ]; then echo -e "${RED}Some tests FAILED.${NC}"; exit 1
else echo -e "${GREEN}All tests PASSED.${NC}"; exit 0; fi
