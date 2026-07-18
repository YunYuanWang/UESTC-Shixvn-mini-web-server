#!/usr/bin/env bash
# ================================================================
# bench_keepalive.sh — keep-alive vs no-keep-alive performance test
#
# Uses Apache Bench (ab):
#   ab -k  → HTTP/1.0 + Connection: Keep-Alive → connection reuse ON
#   ab     → HTTP/1.0, no Connection header → connection close OFF
#
# The server (v1.1) correctly negotiates:
#   HTTP/1.0 → Connection: close   (one request per connection)
#   HTTP/1.1 → Connection: keep-alive (connection reuse)
# ================================================================
set -e

HOST="127.0.0.1"
PORT="9092"
CONCURRENCY=50
TOTAL=10000
CONF="/tmp/d11_bench.conf"
SERVER_LOG="logs/server.log"

# Create temp config
cat > "$CONF" << EOF
host=${HOST}
port=${PORT}
www_root=www
user_file=data/users.csv
log=${SERVER_LOG}
max_connections=512
max_request_bytes=4096
worker_processes=2
worker_shutdown_timeout_ms=3000
EOF

echo "=== Building ==="
make > /dev/null 2>&1

# Clean up
pkill -f "mini_web_server master" 2>/dev/null || true
sleep 0.5
> "$SERVER_LOG"

echo "=== Starting server ==="
./mini_web_server master "$CONF" > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2
echo "Server PID: $SERVER_PID"

# Warm up
curl -s "http://${HOST}:${PORT}/hello" > /dev/null 2>&1 || true

echo ""
echo "============================================"
echo "  Test A: Keep-Alive OFF (HTTP/1.0)"
echo "  ab -c $CONCURRENCY -n $TOTAL"
echo "============================================"
ab -c $CONCURRENCY -n $TOTAL "http://${HOST}:${PORT}/hello" 2>&1 | tee /tmp/ka_off.txt

echo ""
echo "============================================"
echo "  Test B: Keep-Alive ON (HTTP/1.1)"
echo "  ab -k -c $CONCURRENCY -n $TOTAL"
echo "============================================"
ab -k -c $CONCURRENCY -n $TOTAL "http://${HOST}:${PORT}/hello" 2>&1 | tee /tmp/ka_on.txt

echo ""
echo "============================================"
echo "  Performance Comparison"
echo "============================================"

KA_ON_RPS=$(grep "Requests per second" /tmp/ka_on.txt | awk '{print $4}')
KA_OFF_RPS=$(grep "Requests per second" /tmp/ka_off.txt | awk '{print $4}')
KA_ON_TPR=$(grep "Time per request.*mean" /tmp/ka_on.txt | head -1 | awk '{print $4}')
KA_OFF_TPR=$(grep "Time per request.*mean" /tmp/ka_off.txt | head -1 | awk '{print $4}')
KA_ON_FAILED=$(grep "Failed requests" /tmp/ka_on.txt | awk '{print $3}')
KA_OFF_FAILED=$(grep "Failed requests" /tmp/ka_off.txt | awk '{print $3}')
KA_ON_TOTAL=$(grep "Complete requests" /tmp/ka_on.txt | awk '{print $3}')
KA_OFF_TOTAL=$(grep "Complete requests" /tmp/ka_off.txt | awk '{print $3}')

printf "%-25s | %-20s | %-20s | %-15s\n" "Metric" "Keep-Alive ON" "Keep-Alive OFF" "Improvement"
printf "%.0s-" {1..25} && printf "+" && printf "%.0s-" {1..22} && printf "+" && printf "%.0s-" {1..22} && printf "+" && printf "%.0s-" {1..15}
echo ""
printf "%-25s | %-20s | %-20s | %-15s\n" \
    "Throughput (req/s)" "$KA_ON_RPS" "$KA_OFF_RPS" \
    "$(echo "scale=1; $KA_ON_RPS / $KA_OFF_RPS" | bc)x faster"
printf "%-25s | %-20s ms | %-20s ms | %-15s\n" \
    "Avg Latency" "$KA_ON_TPR" "$KA_OFF_TPR" \
    "$(echo "scale=1; $KA_OFF_TPR / $KA_ON_TPR" | bc)x lower"
printf "%-25s | %-20s | %-20s | %-15s\n" \
    "Complete Requests" "$KA_ON_TOTAL" "$KA_OFF_TOTAL" "-"
printf "%-25s | %-20s | %-20s | %-15s\n" \
    "Failed Requests" "$KA_ON_FAILED" "$KA_OFF_FAILED" "-"

# Count keep-alive reuse
KA_REUSE=$(grep -c "keep-alive: waiting" "$SERVER_LOG" 2>/dev/null || echo "0")
echo ""
printf "%-25s | %-20s\n" "Connection Reuses (log)" "$KA_REUSE"

echo ""
echo "=== Stopping server ==="
kill -INT $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null || true
echo ""
echo "Benchmark complete."
