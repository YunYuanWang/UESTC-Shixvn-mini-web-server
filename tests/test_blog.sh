#!/bin/bash
# ================================================================
# Mini Web Server v1.2 — Blog & Logging Test Suite
# ================================================================
#
# Usage: ./tests/test_blog.sh [host] [port]
# Default: 127.0.0.1:8080
#
# Prerequisites: server must be running
#   ./mini_web_server epoll 127.0.0.1 8080
#   or
#   ./mini_web_server master conf/server.conf
# ================================================================

HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
BASE="http://${HOST}:${PORT}"
PASS=0
FAIL=0

green()  { printf "\033[32m%s\033[0m" "$1"; }
red()    { printf "\033[31m%s\033[0m" "$1"; }
yellow() { printf "\033[33m%s\033[0m" "$1"; }

check() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  $(green PASS) $desc"
        PASS=$((PASS + 1))
    else
        echo "  $(red FAIL) $desc (expected: $expected, got: $actual)"
        FAIL=$((FAIL + 1))
    fi
}

check_contains() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if echo "$actual" | grep -q "$expected"; then
        echo "  $(green PASS) $desc"
        PASS=$((PASS + 1))
    else
        echo "  $(red FAIL) $desc (got: $actual)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=========================================="
echo "  Mini Web Server v1.2 — Blog Test Suite"
echo "  Target: ${BASE}"
echo "=========================================="
echo ""

# ================================================================
# Test 1: Blog Homepage (200 + text/html)
# ================================================================
echo "$(yellow '[Test 1]') Blog Homepage (GET /blog/)"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/")
check "Status 200"              "200" "$STATUS"
check_contains "Content-Type text/html" "text/html" "$CTYPE"

# ================================================================
# Test 2: All MIME Types
# ================================================================
echo ""
echo "$(yellow '[Test 2]') MIME Types"

echo "  CSS:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/css/style.css")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/css/style.css")
check "style.css Status 200"          "200" "$STATUS"
check_contains "style.css text/css"   "text/css" "$CTYPE"

echo "  JavaScript:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/js/main.js")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/js/main.js")
check "main.js Status 200"                "200" "$STATUS"
check_contains "main.js application/javascript" "javascript" "$CTYPE"

echo "  PNG:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/img/logo.png")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/img/logo.png")
check "logo.png Status 200"          "200" "$STATUS"
check_contains "logo.png image/png"  "image/png" "$CTYPE"

echo "  JPEG:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/img/banner.jpg")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/img/banner.jpg")
check "banner.jpg Status 200"          "200" "$STATUS"
check_contains "banner.jpg image/jpeg" "image/jpeg" "$CTYPE"

echo "  GIF:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/img/avatar.gif")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/img/avatar.gif")
check "avatar.gif Status 200"         "200" "$STATUS"
check_contains "avatar.gif image/gif" "image/gif" "$CTYPE"

echo "  ICO:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/icon/favicon.ico")
CTYPE=$(curl -s -o /dev/null -w "%{content_type}" "${BASE}/blog/icon/favicon.ico")
check "favicon.ico Status 200"            "200" "$STATUS"
check_contains "favicon.ico image/x-icon" "image/x-icon" "$CTYPE"

# ================================================================
# Test 3: Status Codes
# ================================================================
echo ""
echo "$(yellow '[Test 3]') HTTP Status Codes"

echo "  301 Moved Permanently (/blog -> /blog/):"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog")
check "301 redirect" "301" "$STATUS"

echo "  403 Forbidden (directory traversal):"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" --path-as-is "${BASE}/blog/../etc/passwd")
check "403 forbidden" "403" "$STATUS"

echo "  404 Not Found:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/nonexistent-page.html")
check "404 not found" "404" "$STATUS"

echo "  405 Method Not Allowed:"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${BASE}/blog/index.html")
check "405 method not allowed" "405" "$STATUS"

# ================================================================
# Test 4: Multi-level Directory Structure
# ================================================================
echo ""
echo "$(yellow '[Test 4]') Multi-level Directory Access"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/")
check "blog/ (index)"           "200" "$STATUS"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/about.html")
check "blog/about.html"         "200" "$STATUS"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/article.html")
check "blog/article.html"       "200" "$STATUS"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/blog/README.md")
check "blog/README.md"          "200" "$STATUS"

# ================================================================
# Test 5: Legacy Routes Still Work
# ================================================================
echo ""
echo "$(yellow '[Test 5]') Legacy Routes"
RESP=$(curl -s "${BASE}/hello")
check "GET /hello" "Hello, Web!" "$(echo $RESP)"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/")
check "GET / (www root)" "200" "$STATUS"

# ================================================================
# Summary
# ================================================================
echo ""
echo "=========================================="
echo "  Results: $(green $PASS) passed, $(red $FAIL) failed"
echo "=========================================="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
