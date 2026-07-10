#!/usr/bin/env bash
set -e

cp data/users.csv data/users.csv.bak

rm -f day02_output.txt
rm -f day02_error_output.txt

make clean
make

./mini_web_server findusr ZhangSan > day02_output.txt
grep -xF "FOUND" day02_output.txt

./mini_web_server findusr NonExistent >> day02_output.txt
grep -xF "NOT_FOUND" day02_output.txt

./mini_web_server addusr WangWu,password,20060101,028-00000000,18100000000,WangWu@stdmail.com >> day02_output.txt
grep -xF "ADDED" day02_output.txt

./mini_web_server findusr WangWu >> day02_output.txt
grep -xF "FOUND" day02_output.txt

./mini_web_server delusr WangWu >> day02_output.txt
grep -xF "DELETED" day02_output.txt

./mini_web_server findusr WangWu >> day02_output.txt
grep -xF "NOT_FOUND" day02_output.txt

set +e
./mini_web_server delusr NonExistent > day02_error_output.txt 2>&1
status=$?
set -e

test "$status" -ne 0
grep -xF "NO_SUCH_USER" day02_error_output.txt

set +e
./mini_web_server addusr ZhangSan,password,20060101,028-00000000,18100000000,zhangsan@stdmail.com > day02_error_output.txt 2>&1
status=$?
set -e

test "$status" -ne 0
grep -xF "EXISTS" day02_error_output.txt

set +e
./mini_web_server addusr > day02_error_output.txt 2>&1
status=$?
set -e

test "$status" -ne 0
grep -F "requires an argument" day02_error_output.txt

set +e
./mini_web_server findusr > day02_error_output.txt 2>&1
status=$?
set -e

test "$status" -ne 0
grep -F "requires an argument" day02_error_output.txt

set +e
./mini_web_server delusr > day02_error_output.txt 2>&1
status=$?
set -e

test "$status" -ne 0
grep -F "requires an argument" day02_error_output.txt

rm -f day02_error_output.txt

cp data/users.csv.bak data/users.csv
rm -f data/users.csv.bak

echo "DAY02 PASS"
