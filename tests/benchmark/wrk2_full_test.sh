#!/usr/bin/env bash
# Full wrk2 benchmark for all 4 modes (fork/thread/pool/select)
set +e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULT_DIR="$SCRIPT_DIR/results/wrk2"
mkdir -p "$RESULT_DIR"

# Test config
CONN_LEVELS="10 50 100 200 500 800 1000"
DURATION=10
BASE_PORT=9500

cd "$PROJECT_DIR"
make clean > /dev/null 2>&1
make > /dev/null 2>&1

echo "mode,concurrency,qps,avg_latency_ms,max_latency_ms,p50_ms,p75_ms,p90_ms,p95_ms,p99_ms,total_requests,errors" > "$RESULT_DIR/all_results.csv"

for MODE in thread pool select fork; do
  for CONN in $CONN_LEVELS; do
    # Fork/thread safety limits
    if [ "$MODE" = "fork" ] && [ "$CONN" -gt 500 ]; then continue; fi

    PORT=$((BASE_PORT + RANDOM % 4000))

    # Kill old, start new
    pkill -9 -f "mini_web_server" 2>/dev/null || true
    sleep 0.3
    fuser -k ${PORT}/tcp 2>/dev/null || true
    sleep 0.2
    rm -f logs/server.log

    ./mini_web_server "$MODE" 127.0.0.1 "$PORT" > /dev/null 2>&1 &
    SPID=$!
    sleep 0.5

    if ! kill -0 $SPID 2>/dev/null; then
      echo "$MODE,$CONN,0,0,0,0,0,0,0,0,0,server_crashed" >> "$RESULT_DIR/all_results.csv"
      continue
    fi

    # Warmup
    curl -s --max-time 2 "http://127.0.0.1:${PORT}/hello" > /dev/null 2>&1 || true

    # wrk2 test
    THREADS=2
    RATE=200000
    OUTFILE="$RESULT_DIR/wrk_${MODE}_c${CONN}.log"

    timeout $((DURATION + 15)) wrk2 -t$THREADS -c$CONN -d${DURATION}s -R$RATE --latency \
      "http://127.0.0.1:${PORT}/hello" > "$OUTFILE" 2>&1
    RC=$?

    # Parse
    if [ $RC -eq 124 ]; then
      echo "$MODE,$CONN,0,0,0,0,0,0,0,0,0,timeout" >> "$RESULT_DIR/all_results.csv"
      echo "  $MODE c=$CONN: TIMEOUT"
    elif grep -q "Requests/sec" "$OUTFILE" 2>/dev/null; then
      QPS=$(grep "Requests/sec:" "$OUTFILE" | awk '{print $2}')
      AVG=$(grep -E "^\s+Latency\s" "$OUTFILE" | awk '{print $2}' | sed 's/us//;s/ms//;s/s//')
      MAX=$(grep -E "^\s+Latency\s" "$OUTFILE" | awk '{print $4}' | sed 's/us//;s/ms//;s/s//')
      TOTAL=$(grep "requests in" "$OUTFILE" | awk '{print $1}')
      ERR=$(grep -c "Socket errors\|Non-2xx" "$OUTFILE" 2>/dev/null || echo 0)

      # percentiles
      get_pct() { grep -E "^\s*${1}%" "$OUTFILE" 2>/dev/null | head -1 | awk '{print $2}' | sed 's/us//;s/ms//;s/s//'; }
      P50=$(get_pct "50.000"); P75=$(get_pct "75.000"); P90=$(get_pct "90.000")
      P95=$(get_pct "95.000"); P99=$(get_pct "99.000")

      echo "$MODE,$CONN,$QPS,$AVG,$MAX,$P50,$P75,$P90,$P95,$P99,$TOTAL,$ERR" >> "$RESULT_DIR/all_results.csv"
      echo "  $MODE c=$CONN: QPS=$QPS avg=${AVG}ms P95=$P95"
    else
      echo "$MODE,$CONN,0,0,0,0,0,0,0,0,0,no_result" >> "$RESULT_DIR/all_results.csv"
      echo "  $MODE c=$CONN: no valid result"
    fi

    pkill -9 -f "mini_web_server" 2>/dev/null || true
    sleep 2
  done
  echo "--- $MODE complete ---"
done

pkill -9 -f "mini_web_server" 2>/dev/null || true
echo ""
echo "Results: $RESULT_DIR/all_results.csv"
