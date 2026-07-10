#!/usr/bin/env bash
set -e

cp data/users.csv data/users.csv.bak

rm -f day03_output.txt
rm -f day03_index_output.txt
rm -f day03_error_output.txt
rm -f day03_compare_output.txt
rm -f logs/server.log

make clean
make

# -------------------------------------------------------
# Test 1: users index - sorted inorder output
# -------------------------------------------------------
echo "--- Test 1: users index ---"
./mini_web_server users index > day03_index_output.txt

# Verify all users appear in alphabetical order
grep -F "ChenQi," day03_index_output.txt
grep -F "LiSi," day03_index_output.txt
grep -F "SunBa," day03_index_output.txt
grep -F "WuShi," day03_index_output.txt
grep -F "ZhangSan," day03_index_output.txt
grep -F "ZhaoLiu," day03_index_output.txt
grep -F "ZhouJiu," day03_index_output.txt

# Verify sorted order: ChenQi < LiSi < SunBa < WuShi < ZhangSan < ZhaoLiu < ZhouJiu
prev=""
while IFS=',' read -r name rest; do
    if [ -n "$prev" ]; then
        if [[ "$prev" > "$name" ]]; then
            echo "FAIL: index output not sorted: $prev > $name"
            exit 1
        fi
    fi
    prev="$name"
done < day03_index_output.txt

echo "PASS: users index (sorted order verified)"

# -------------------------------------------------------
# Test 2: users find-index - BST search
# -------------------------------------------------------
echo "--- Test 2: users find-index ---"

./mini_web_server users find-index ZhangSan > day03_output.txt
grep -xF "FOUND" day03_output.txt

./mini_web_server users find-index NonExistent > day03_output.txt
grep -xF "NOT_FOUND" day03_output.txt

echo "PASS: users find-index"

# -------------------------------------------------------
# Test 3: users compare_search_method
# -------------------------------------------------------
echo "--- Test 3: users compare_search_method ---"

./mini_web_server users compare_search_method ZhangSan > day03_compare_output.txt

grep -F "Search Method Comparison" day03_compare_output.txt
grep -F "Target user: ZhangSan" day03_compare_output.txt
grep -F "[Single Search]" day03_compare_output.txt
grep -F "Linked list:" day03_compare_output.txt
grep -F "BST index:" day03_compare_output.txt
grep -F "[Continuous Search Benchmark" day03_compare_output.txt

echo "PASS: users compare_search_method"

# -------------------------------------------------------
# Test 4: BST sync with addusr (linked list + BST)
# -------------------------------------------------------
echo "--- Test 4: BST sync with addusr ---"

./mini_web_server addusr TestBST,pass123,20000101,010-11111111,13900000000,testbst@test.com > day03_output.txt
grep -xF "ADDED" day03_output.txt

# Verify linked list find works
./mini_web_server findusr TestBST > day03_output.txt
grep -xF "FOUND" day03_output.txt

# Verify BST index find also works
./mini_web_server users find-index TestBST > day03_output.txt
grep -xF "FOUND" day03_output.txt

# Verify it appears in index output
./mini_web_server users index > day03_index_output.txt
grep -F "TestBST," day03_index_output.txt

echo "PASS: BST sync with addusr"

# -------------------------------------------------------
# Test 5: BST sync with delusr (BST removed before list free)
# -------------------------------------------------------
echo "--- Test 5: BST sync with delusr ---"

./mini_web_server delusr TestBST > day03_output.txt
grep -xF "DELETED" day03_output.txt

# Verify linked list find no longer finds it
./mini_web_server findusr TestBST > day03_output.txt
grep -xF "NOT_FOUND" day03_output.txt

# Verify BST index find also no longer finds it
./mini_web_server users find-index TestBST > day03_output.txt
grep -xF "NOT_FOUND" day03_output.txt

# Verify it does NOT appear in index output
./mini_web_server users index > day03_index_output.txt
set +e
grep -F "TestBST," day03_index_output.txt > /dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "FAIL: deleted user still appears in BST index"
    exit 1
fi
set -e

echo "PASS: BST sync with delusr (no double-free)"

# -------------------------------------------------------
# Test 6: Log timestamps
# -------------------------------------------------------
echo "--- Test 6: log timestamps ---"

./mini_web_server conf/server.conf > /dev/null 2>&1

# Verify INFO entries have timestamps in format [YYYY-MM-DD HH:MM:SS.xxxxxx]
grep -E "\[INFO\] \[PID [0-9]+\] \[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6}\]" logs/server.log

echo "PASS: log timestamps"

# -------------------------------------------------------
# Test 7: Error handling for users subcommands
# -------------------------------------------------------
echo "--- Test 7: error handling ---"

# users without subcommand
set +e
./mini_web_server users > day03_error_output.txt 2>&1
status=$?
set -e
test "$status" -ne 0
grep -F "requires a subcommand" day03_error_output.txt

# users find-index without name
set +e
./mini_web_server users find-index > day03_error_output.txt 2>&1
status=$?
set -e
test "$status" -ne 0
grep -F "requires a name argument" day03_error_output.txt

# users compare_search_method without name
set +e
./mini_web_server users compare_search_method > day03_error_output.txt 2>&1
status=$?
set -e
test "$status" -ne 0
grep -F "requires a name argument" day03_error_output.txt

# users with unknown subcommand
set +e
./mini_web_server users unknown_cmd > day03_error_output.txt 2>&1
status=$?
set -e
test "$status" -ne 0
grep -F "unknown users subcommand" day03_error_output.txt

echo "PASS: error handling"

# -------------------------------------------------------
# Test 8: Existing commands still work (regression)
# -------------------------------------------------------
echo "--- Test 8: regression ---"

./mini_web_server findusr ZhangSan > day03_output.txt
grep -xF "FOUND" day03_output.txt

./mini_web_server findusr NonExistent > day03_output.txt
grep -xF "NOT_FOUND" day03_output.txt

echo "PASS: regression"

# -------------------------------------------------------
# Cleanup
# -------------------------------------------------------
rm -f day03_output.txt
rm -f day03_index_output.txt
rm -f day03_error_output.txt
rm -f day03_compare_output.txt
rm -f logs/server.log

cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo ""
echo "DAY03 PASS"
