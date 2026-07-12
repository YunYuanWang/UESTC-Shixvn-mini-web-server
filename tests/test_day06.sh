#!/usr/bin/env bash
set -e

# ================================================================
# test_day06.sh — TCP/HTTP server tests (v0.6)
#
# Tests:
#   1. GET /hello          — 200 OK with "Hello, Web!"
#   2. GET /users/<name>   — 200 OK with user info
#   3. GET /user/<name>    — 200 OK with user info
#   4. GET /not-exist      — 404 NOT FOUND
#   5. GET /user/NoOne     — 404 NOT FOUND (user not in store)
#   6. POST /users         — 200 OK ADDED
#   7. DELETE /users/<name>— 200 OK DELETED
#   8. GET /users          — 200 OK list all
# ================================================================

HOST="127.0.0.1"
PORT="8080"
BASE="http://${HOST}:${PORT}"

# backup data
cp data/users.csv data/users.csv.bak

# build
make clean > /dev/null 2>&1
make > /dev/null 2>&1

echo "=== Test 1: GET /hello ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/hello" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "Hello, Web!"
echo "PASS: GET /hello"

echo "=== Test 2: GET /users/ZhangSan ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/users/ZhangSan" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "name: ZhangSan"
echo "$RESP" | grep -F "email: zhangsan@stdmail.com"
echo "PASS: GET /users/ZhangSan"

echo "=== Test 3: GET /user/LiSi ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/user/LiSi" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "name: LiSi"
echo "PASS: GET /user/LiSi"

echo "=== Test 4: GET /not-exist (404) ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/not-exist" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 404 NOT FOUND"
echo "$RESP" | grep -F "404 Not Found"
echo "PASS: GET /not-exist"

echo "=== Test 5: GET /user/NoOne (404) ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/user/NoOne" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 404 NOT FOUND"
echo "$RESP" | grep -F "NOT_FOUND NoOne"
echo "PASS: GET /user/NoOne"

echo "=== Test 6: POST /users (add user) ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i -X POST -d "Day06Test,pass666,20000315,010-66666666,13900000000,day06@test.com" "${BASE}/users" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "ADDED"
echo "PASS: POST /users"

# verify user was added by looking it up
echo "=== Test 6b: verify added user ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/users/Day06Test" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "name: Day06Test"
echo "PASS: verify added user"

echo "=== Test 7: DELETE /users/Day06Test ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i -X DELETE "${BASE}/users/Day06Test" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "DELETED"
echo "PASS: DELETE /users/Day06Test"

echo "=== Test 8: GET /users (list all) ==="
./mini_web_server conf/server.conf &
sleep 0.3
RESP=$(curl -s -i "${BASE}/users" 2>&1 || true)
wait
echo "$RESP" | grep -F "HTTP/1.1 200 OK"
echo "$RESP" | grep -F "ZhangSan"
echo "$RESP" | grep -F "LiSi"
echo "PASS: GET /users"

# ================================================================
# Cleanup
# ================================================================
rm -f logs/server.log
cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY06 PASS"
