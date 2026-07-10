#!/usr/bin/env bash
set -e

rm -f logs/server.log
rm -f day01_output.txt
rm -f bad_log_output.txt
rm -f conf/bad_log.conf

make clean
make

./mini_web_server conf/server.conf > day01_output.txt

grep -F "server_name=MiniWeb" day01_output.txt
grep -F "host=127.0.0.1" day01_output.txt
grep -F "port=8080" day01_output.txt
grep -F "root=www" day01_output.txt
grep -F "log=logs/server.log" day01_output.txt
grep -F "HTTP/1.1 200 OK" day01_output.txt
grep -F "Content-Type: text/html" day01_output.txt
grep -F "Content-Length: 12" day01_output.txt
grep -F "Hello, Web!" day01_output.txt

test -f logs/server.log
grep -F "server config loaded" logs/server.log
grep -F "document root loaded" logs/server.log

cat > conf/bad_log.conf <<'CONFIG'
server_name=BadLog
host=127.0.0.1
port=8080
root=www
log=logs/missing/server.log
CONFIG

set +e
./mini_web_server conf/bad_log.conf > bad_log_output.txt 2>&1
status=$?
set -e

test "$status" -ne 0
grep -F "failed to open log file" bad_log_output.txt

rm -f conf/bad_log.conf

echo "DAY01 PASS"
