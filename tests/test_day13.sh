#!/usr/bin/env bash
set -e

# ================================================================
# test_day13.sh — POST /search 状态码测试 (v1.4)
#
# 使用 nc (netcat) 发送原始 HTTP 请求，精确控制请求头，
# 测试所有 POST /search 端点可能返回的状态码。
#
# 测试用例:
#   1. 200 OK — 正常中文搜索
#   2. 200 OK — 搜索页面 GET /
#   3. 200 OK — 空查询返回表单
#   4. 200 OK — 无结果搜索
#   5. 400 Bad Request — 请求行格式错误（无路径）
#   6. 400 Bad Request — Content-Length 缺失
#   7. 400 Bad Request — Body 长度与 Content-Length 不一致
#   8. 400 Bad Request — URL 编码错误 (%ZZ)
#   9. 413 Payload Too Large — 请求体超过 max_request_bytes
#  10. 415 Unsupported Media Type — 错误的 Content-Type
#  11. 404 Not Found — 不存在的路径
# ================================================================

HOST="127.0.0.1"
PORT="9180"
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

# Helper: send a raw HTTP request via nc and capture the response
# Usage: nc_send "RAW_HTTP_REQUEST"
# Note: uses printf '%b' so that \r\n escape sequences become real CRLF bytes.
# Returns: HTTP response (stdout)
nc_send() {
    printf '%b' "$1" | nc -w 2 ${HOST} ${PORT} 2>/dev/null || true
}

# Helper: send a raw HTTP request and extract the status line
# Usage: nc_status "RAW_HTTP_REQUEST"
# Returns: e.g. "HTTP/1.1 200 OK"
nc_status() {
    nc_send "$1" | head -1 | tr -d '\r'
}

# Helper: send a raw HTTP request and check the status code
# Usage: check_status EXPECTED_CODE DESCRIPTION "RAW_REQUEST"
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

# Helper: send a raw HTTP request and check response body contains text
# Usage: check_body CONTAINS_TEXT DESCRIPTION "RAW_REQUEST"
check_body() {
    local contain="$1"
    local desc="$2"
    local request="$3"
    local resp

    resp=$(nc_send "$request")
    if echo "$resp" | grep -qF "$contain"; then
        pass "$desc"
    else
        fail "$desc — body missing '$contain'"
    fi
}

# ---- Setup ----
echo "=============================================="
echo " test_day13.sh — POST /search Status Code Tests"
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

# Create temp config with Nginx-style format for virtual host support
cat > /tmp/d13_server.conf << 'CONFEOF'
worker_processes 1
worker_shutdown_timeout_ms 2000
max_connections 256
max_request_bytes 4096
user_file data/users_100000_c.csv
log logs/server.log

server {
    listen 127.0.0.1:9180 default_server
    server_name users.localhost
    root search
}
CONFEOF

# Start server
./mini_web_server master /tmp/d13_server.conf > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FATAL: Server failed to start"
    exit 1
fi
info "Server PID=$SERVER_PID started on port $PORT"

# ---- Test 1: 200 OK — Normal Chinese search ----
echo ""
echo "--- Test 1: 200 OK — Chinese search ---"
check_status "HTTP/1.1 200" \
    "Chinese name search '赵安'" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 11\r\n\r\nname=赵安"

check_body "赵安安" \
    "Result contains 赵安安" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 11\r\n\r\nname=赵安"

check_body "result-card" \
    "Result contains result-card div" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 11\r\n\r\nname=赵安"

# ---- Test 2: 200 OK — Search page GET ----
echo ""
echo "--- Test 2: 200 OK — Search page GET / ---"
check_status "HTTP/1.1 200" \
    "GET / search page" \
    "GET / HTTP/1.1\r\nHost: users.localhost\r\n\r\n"

check_body "User Manager" \
    "Page contains 'User Manager'" \
    "GET / HTTP/1.1\r\nHost: users.localhost\r\n\r\n"

# ---- Test 3: 200 OK — Empty query returns form ----
echo ""
echo "--- Test 3: 200 OK — Empty criteria ---"
check_status "HTTP/1.1 200" \
    "Empty criteria returns form" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 5\r\n\r\nname="

check_body "Please enter at least one search criterion" \
    "Form shows warning for empty criteria" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 5\r\n\r\nname="

# ---- Test 4: 200 OK — No results found ----
echo ""
echo "--- Test 4: 200 OK — No results ---"
check_status "HTTP/1.1 200" \
    "Search with no matches" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 18\r\n\r\nname=zzzxyznomatch"

check_body "No users found" \
    "Shows 'No users found' message" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 18\r\n\r\nname=zzzxyznomatch"

# ---- Test 5: 400 Bad Request — Malformed request line ----
echo ""
echo "--- Test 5: 400 Bad Request — Malformed line ---"
check_status "HTTP/1.1 400" \
    "Request line without path" \
    "GET\r\nHost: users.localhost\r\n\r\n"

# ---- Test 6: 400 Bad Request — Missing Content-Length ----
echo ""
echo "--- Test 6: 400 Bad Request — Missing Content-Length ---"
check_status "HTTP/1.1 400" \
    "POST without Content-Length header" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\nname=test"

# ---- Test 7: 400 Bad Request — Content-Length mismatch ----
echo ""
echo "--- Test 7: 400 Bad Request — Content-Length mismatch ---"
check_status "HTTP/1.1 400" \
    "Body(9) != Content-Length(999)" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 999\r\n\r\nname=test"

# ---- Test 8: 400 Bad Request — URL encoding error ----
echo ""
echo "--- Test 8: 400 Bad Request — URL encoding error ---"
check_status "HTTP/1.1 400" \
    "Invalid %ZZ in name parameter" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 13\r\n\r\nname=%ZZtest"

# ---- Test 9: 413 Payload Too Large ----
echo ""
echo "--- Test 9: 413 Payload Too Large ---"
# Generate a large body (~4100 bytes) to exceed max_request_bytes (4096)
LARGE_BODY=$(python3 -c "print('x'*4100)" 2>/dev/null || head -c 4100 /dev/zero | tr '\0' 'x')
LARGE_REQ="POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 4105\r\n\r\nname=${LARGE_BODY}"
check_status "HTTP/1.1 413" \
    "Request body > 4096 bytes" \
    "$LARGE_REQ"

# ---- Test 10: 415 Unsupported Media Type ----
echo ""
echo "--- Test 10: 415 Unsupported Media Type ---"
check_status "HTTP/1.1 415" \
    "Content-Type: text/plain" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n\r\nname=test"

# ---- Test 11: 200 OK — Multi-field AND search ----
echo ""
echo "--- Test 11: 200 OK — AND search (name + phone) ---"
check_status "HTTP/1.1 200" \
    "AND search: name=赵安 + phone=138" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 21\r\n\r\nname=赵安&phone=138"

check_body "赵安" \
    "AND result contains matching name" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 21\r\n\r\nname=赵安&phone=138"

# ---- Test 12: 200 OK — Email search ----
echo ""
echo "--- Test 12: 200 OK — Email search ---"
check_status "HTTP/1.1 200" \
    "Search by email domain" \
    "POST /search HTTP/1.1\r\nHost: users.localhost\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 18\r\n\r\nemail=@example.com"

# ---- Test 13: 404 Not Found ----
echo ""
echo "--- Test 11: 404 Not Found ---"
check_status "HTTP/1.1 404" \
    "GET /nonexistent/path" \
    "GET /nonexistent/path HTTP/1.1\r\nHost: users.localhost\r\n\r\n"

# ---- Cleanup ----
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

# Cleanup temp config
rm -f /tmp/d13_server.conf

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo -e "${RED}Some tests FAILED. Check output above.${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}All tests PASSED.${NC}"
    exit 0
fi
