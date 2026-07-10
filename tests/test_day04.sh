#!/usr/bin/env bash
set -e

cp data/users.csv data/users.csv.bak

rm -f logs/server.log
rm -f outputs/*.out
rm -f requests/malformed.req

make clean
make

# verify compilation succeeded
test -f mini_web_server
test -f request_worker

# --- Test 1: basic request types (hello, find, missing) ---
echo "--- Test 1: basic requests ---"

./mini_web_server process > /dev/null 2>&1

test -f outputs/hello.out
test -f outputs/user_find.out
test -f outputs/missing.out

grep -F "Hello, Web!" outputs/hello.out
grep -F "FOUND ZhangSan 123456 13800000001" outputs/user_find.out
# user_find.req has two lines: ZhangSan (exists) and WuLiuqi (does not)
grep -F "NOT_FOUND WuLiuqi" outputs/user_find.out
grep -F -- "---" outputs/user_find.out
grep -F "404 Not Found: GET /nonexistent" outputs/missing.out

echo "PASS: basic requests"

# --- Test 2: log PID ---
echo "--- Test 2: log PID ---"

test -f logs/server.log
grep -E "\[PID [0-9]+\]" logs/server.log > /dev/null
grep -F "[ProcessServer] scanning requests" logs/server.log
grep -F "======" logs/server.log | grep -F "Child" | grep -F "started" > /dev/null
grep -F "[ProcessServer] all children processed" logs/server.log
grep -F "[Worker] processing request" logs/server.log
grep -F "[Worker] request processed (" logs/server.log

echo "PASS: log PID"

# --- Test 3: GET /users (list all) ---
echo "--- Test 3: GET /users ---"

cat > requests/users_index.req <<'REQ'
GET /users
REQ

./mini_web_server process > /dev/null 2>&1

grep -F "ChenQi," outputs/users_index.out
grep -F "ZhangSan," outputs/users_index.out
grep -F "ZhouJiu," outputs/users_index.out

rm -f requests/users_index.req outputs/users_index.out
echo "PASS: GET /users"

# --- Test 4: GET /users/find-index/<name> ---
echo "--- Test 4: GET /users/find-index ---"

cat > requests/users_find_index.req <<'REQ'
GET /users/find-index/ZhangSan
REQ

./mini_web_server process > /dev/null 2>&1

grep -F "FOUND ZhangSan" outputs/users_find_index.out

rm -f requests/users_find_index.req outputs/users_find_index.out
echo "PASS: GET /users/find-index"

# --- Test 5: GET /users/compare/<name> ---
echo "--- Test 5: GET /users/compare ---"

cat > requests/users_compare.req <<'REQ'
GET /users/compare/ZhangSan
REQ

./mini_web_server process > /dev/null 2>&1

grep -F "Search Method Comparison" outputs/users_compare.out
grep -F "[Single Search]" outputs/users_compare.out
grep -F "Linked list:" outputs/users_compare.out
grep -F "BST index:" outputs/users_compare.out

rm -f requests/users_compare.req outputs/users_compare.out
echo "PASS: GET /users/compare"

# --- Test 6: POST /users (add user) ---
echo "--- Test 6: POST /users ---"

cat > requests/add_user.req <<'REQ'
POST /users
ReqTest,pass999,20000101,010-99999999,13900000000,reqtest@test.com
REQ

./mini_web_server process > /dev/null 2>&1

grep -F "ADDED" outputs/add_user.out

# verify the user was actually added
./mini_web_server findusr ReqTest > /dev/null 2>&1
grep -xF "FOUND" day02_output.txt

rm -f requests/add_user.req outputs/add_user.out
echo "PASS: POST /users"

# --- Test 7: DELETE /users/<name> ---
echo "--- Test 7: DELETE /users ---"

cat > requests/delete_user.req <<'REQ'
DELETE /users/ReqTest
REQ

./mini_web_server process > /dev/null 2>&1

grep -F "DELETED" outputs/delete_user.out

# verify the user was actually deleted
./mini_web_server findusr ReqTest > /dev/null 2>&1
grep -xF "NOT_FOUND" day02_output.txt

rm -f requests/delete_user.req outputs/delete_user.out
echo "PASS: DELETE /users"

# --- Test 8: NOT_FOUND user ---
echo "--- Test 8: NOT_FOUND user ---"

cat > requests/notfound.req <<'REQ'
GET /user/NonExistentUser
REQ

./mini_web_server process > /dev/null 2>&1
grep -F "NOT_FOUND NonExistentUser" outputs/notfound.out
rm -f requests/notfound.req outputs/notfound.out

echo "PASS: NOT_FOUND user"

# --- Test 9: malformed request ---
echo "--- Test 9: malformed request ---"

cat > requests/malformed.req <<'REQ'
INVALID
REQ

./mini_web_server process > /dev/null 2>&1
grep -F "[Worker] malformed request" logs/server.log
rm -f requests/malformed.req

echo "PASS: malformed request"

# --- Test 10: regression ---
echo "--- Test 10: regression ---"

rm -f logs/server.log

./mini_web_server findusr ZhangSan > /dev/null 2>&1
grep -xF "FOUND" day02_output.txt

./mini_web_server conf/server.conf > /dev/null 2>&1
grep -F "[INFO]" logs/server.log

echo "PASS: regression"

# --- Cleanup ---
rm -f logs/server.log
rm -f outputs/*.out

cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY04 PASS"
