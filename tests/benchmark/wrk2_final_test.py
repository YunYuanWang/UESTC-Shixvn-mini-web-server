#!/usr/bin/env python3
"""
Pure wrk2 benchmark — find performance limits for all 4 modes.

Key design: use very high -R (10M) to eliminate wrk2 rate-limiting.
The server saturates naturally — QPS ceiling and latency knee are
the real performance limits.

Signals of hitting the limit:
  - QPS stops growing despite higher concurrency → throughput ceiling
  - P50/P95/P99 spike dramatically → latency knee
  - Socket errors appear → connection rejection
  - Non-2xx responses appear → server returning errors
"""

import subprocess, time, os, signal, re, json, sys

PROJECT = "/home/zigh-wang/data-disk/shixvn/miniwebserver"
os.chdir(PROJECT)

def kill_all():
    subprocess.run(["pkill", "-9", "-f", "mini_web_server"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)

def build():
    subprocess.run(["make", "clean"], capture_output=True)
    subprocess.run(["make"], capture_output=True)

def _to_ms(val, unit):
    if unit == "us": return val / 1000.0
    if unit == "s": return val * 1000.0
    return float(val)

def parse_wrk2(output):
    """Parse wrk2 output, return dict of metrics."""
    r = {}
    m = re.search(r"Requests/sec:\s+([\d.]+)", output)
    if m: r["qps"] = float(m.group(1))

    m = re.search(r"(\d+)\s+requests in\s+([\d.]+)s", output)
    if m:
        r["total"] = int(m.group(1))
        r["duration_s"] = float(m.group(2))

    # Latency stats: "    Latency   108.66ms  128.78ms 505.60ms   81.32%"
    m = re.search(r"Latency\s+([\d.]+)(ms|us|s)\s+([\d.]+)(ms|us|s)\s+([\d.]+)(ms|us|s)",
                  output)
    if m:
        r["avg_ms"] = _to_ms(float(m.group(1)), m.group(2))
        r["stdev_ms"] = _to_ms(float(m.group(3)), m.group(4))
        r["max_ms"] = _to_ms(float(m.group(5)), m.group(6))

    # Percentiles: " 50.000%   49.38ms"
    for pct in ["50.000", "75.000", "90.000", "95.000", "99.000", "99.900"]:
        pat = r"%s%%\s+([\d.]+)(ms|us|s)" % pct.replace(".", r"\.")
        m = re.search(pat, output)
        if m:
            key = "p%s_ms" % pct.replace(".", "_")
            r[key] = _to_ms(float(m.group(1)), m.group(2))

    # Socket errors
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)",
                  output)
    if m:
        r["err_connect"] = int(m.group(1))
        r["err_read"] = int(m.group(2))
        r["err_write"] = int(m.group(3))
        r["err_timeout"] = int(m.group(4))

    # Non-2xx
    m = re.search(r"Non-2xx or 3xx responses: (\d+)", output)
    if m: r["non_2xx"] = int(m.group(1))

    return r

def run_test(mode, port, conn, duration=15):
    """Run a single wrk2 test. Returns (metrics, raw_output)."""
    kill_all()
    subprocess.run(["rm", "-f", "logs/server.log"], capture_output=True)

    proc = subprocess.Popen(
        ["./mini_web_server", mode, "127.0.0.1", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)

    if proc.poll() is not None:
        proc.wait()
        return {"error": "server_crashed"}, ""

    threads = 4
    # KEY INSIGHT: wrk2 -R is TOTAL rate. Each connection handles R/c req/s.
    # If R/c > ~1000 (per-conn max for this server), wrk2 rate-limiter breaks.
    # Formula: R = conn * 1000, capped at 300K
    rate = conn * 1000
    if rate < 20000: rate = 20000
    if rate > 300000: rate = 300000

    try:
        r = subprocess.run(
            ["wrk2", f"-t{threads}", f"-c{conn}", f"-d{duration}s",
             f"-R{rate}", "--latency",
             f"http://127.0.0.1:{port}/hello"],
            capture_output=True, text=True, timeout=duration + 15)
        output = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGKILL)
        proc.wait()
        return {"error": "wrk2_timeout"}, ""

    proc.send_signal(signal.SIGKILL)
    proc.wait()
    time.sleep(0.3)

    metrics = parse_wrk2(output)
    return metrics, output


def build_table(results, mode, levels, output_dir):
    """Build result table and save wrk2 outputs for one mode."""
    # Save raw wrk2 outputs
    out_dir = f"{output_dir}/{mode}"
    os.makedirs(out_dir, exist_ok=True)

    print(f"\n{'='*90}")
    print(f"  {mode.upper()} MODE — wrk2 Benchmark Results (-R 10M)")
    print(f"{'='*90}")
    print(f"{'conn':>6} {'QPS':>9} {'avg_ms':>8} {'P50':>8} {'P90':>8} "
          f"{'P95':>8} {'P99':>8} {'P99.9':>8} {'max_ms':>8}  "
          f"{'errs':>5} {'total':>8}")
    print("-" * 95)

    table_rows = []
    for conn in levels:
        metrics, raw = results.get(conn, ({}, ""))

        # Save raw output
        with open(f"{out_dir}/c{conn}.log", "w") as f:
            f.write(raw)

        if "error" in metrics:
            print(f"{conn:>6}  ERROR: {metrics['error']}")
            table_rows.append([conn, "ERROR", metrics["error"], "", "", "", "", "", "", ""])
            continue

        qps = metrics.get("qps", 0)
        avg = metrics.get("avg_ms", 0)
        p50 = metrics.get("p50_000_ms", 0)
        p90 = metrics.get("p90_000_ms", 0)
        p95 = metrics.get("p95_000_ms", 0)
        p99 = metrics.get("p99_000_ms", 0)
        p999 = metrics.get("p99_900_ms", 0)
        mx = metrics.get("max_ms", 0)
        errs = (metrics.get("err_connect", 0) + metrics.get("err_read", 0) +
                metrics.get("err_write", 0) + metrics.get("err_timeout", 0) +
                metrics.get("non_2xx", 0))
        total = metrics.get("total", 0)

        print(f"{conn:>6} {qps:>9.0f} {avg:>8.2f} {p50:>8.2f} {p90:>8.2f} "
              f"{p95:>8.2f} {p99:>8.2f} {p999:>8.2f} {mx:>8.2f}  "
              f"{errs:>5} {total:>8}")
        table_rows.append([conn, qps, avg, p50, p90, p95, p99, p999, mx, errs, total])

    return table_rows


def main():
    build()

    output_dir = f"{PROJECT}/tests/benchmark/results/wrk2_final"
    os.makedirs(output_dir, exist_ok=True)

    # Test matrix: each mode at increasing concurrency until limit
    configs = {
        "select": [10, 50, 100, 200, 500, 800, 1000, 1100, 1200],
        "pool":   [10, 50, 100, 200, 500, 800, 1000],
        "thread": [10, 50, 100, 200, 500, 800, 1000, 1200],
        "fork":   [10, 50, 100, 200, 500],
    }

    all_data = {}
    for mode in ["select", "pool", "thread", "fork"]:
        levels = configs[mode]
        results = {}
        for conn in levels:
            port = 9500 + hash(mode) % 100 + conn % 100
            print(f"\r{mode} c={conn}: testing...", end="", flush=True)
            metrics, raw = run_test(mode, port, conn)
            results[conn] = (metrics, raw)
            # Quick check: if QPS is dropping or errors piling up, we've hit the limit
            qps = metrics.get("qps", 0)
            total_errs = (metrics.get("err_connect", 0) + metrics.get("err_read", 0) +
                         metrics.get("err_write", 0) + metrics.get("err_timeout", 0) +
                         metrics.get("non_2xx", 0))
            time.sleep(1)

        print(f"\r{mode}: building table...", end="", flush=True)
        build_table(results, mode, levels, output_dir)
        all_data[mode] = results

    # Save aggregated CSV for plot_results.py
    csv_path = f"{output_dir}/all_results.csv"
    with open(csv_path, "w") as f:
        f.write("mode,concurrency,qps,avg_ms,p50_ms,p75_ms,p90_ms,p95_ms,p99_ms,p99_9_ms,max_ms,errors,total\n")
        for mode, results in all_data.items():
            for conn, (metrics, _) in results.items():
                if "error" in metrics: continue
                f.write(f"{mode},{conn},"
                       f"{metrics.get('qps',0)},"
                       f"{metrics.get('avg_ms',0)},"
                       f"{metrics.get('p50_000_ms',0)},"
                       f"{metrics.get('p75_000_ms',0)},"
                       f"{metrics.get('p90_000_ms',0)},"
                       f"{metrics.get('p95_000_ms',0)},"
                       f"{metrics.get('p99_000_ms',0)},"
                       f"{metrics.get('p99_900_ms',0)},"
                       f"{metrics.get('max_ms',0)},"
                       f"{metrics.get('err_connect',0) + metrics.get('err_read',0) + metrics.get('err_write',0) + metrics.get('err_timeout',0) + metrics.get('non_2xx',0)},"
                       f"{metrics.get('total',0)}\n")

    print(f"\n\nAll results saved to: {csv_path}")
    print(f"Raw wrk2 outputs in: {output_dir}/<mode>/c<conn>.log")

    # Print limit analysis
    print("\n" + "=" * 60)
    print("  PERFORMANCE LIMIT ANALYSIS")
    print("=" * 60)
    for mode in ["select", "pool", "thread", "fork"]:
        results = all_data.get(mode, {})
        if not results: continue
        # Find: max QPS, latency knee (where P95 starts spiking)
        max_qps = 0; max_qps_conn = 0; knee_conn = None
        for conn, (metrics, _) in sorted(results.items()):
            if "error" in metrics: continue
            qps = metrics.get("qps", 0)
            if qps > max_qps:
                max_qps = qps; max_qps_conn = conn
            p95 = metrics.get("p95_000_ms", 0)
            if p95 > 50 and knee_conn is None:  # P95 > 50ms = latency knee
                knee_conn = conn

        print(f"\n  {mode.upper()}:")
        print(f"    Max QPS: {max_qps:.0f} @ c={max_qps_conn}")
        if knee_conn:
            m = results[knee_conn][0]
            print(f"    Latency knee: c={knee_conn} (P50={m.get('p50_000_ms',0):.1f}ms P95={m.get('p95_000_ms',0):.1f}ms P99={m.get('p99_000_ms',0):.1f}ms)")
        # Check for errors
        error_conns = []
        for conn, (metrics, _) in sorted(results.items()):
            total_errs = (metrics.get("err_connect", 0) + metrics.get("err_read", 0) +
                         metrics.get("err_write", 0) + metrics.get("err_timeout", 0) +
                         metrics.get("non_2xx", 0))
            if total_errs > 0:
                error_conns.append((conn, total_errs))
        if error_conns:
            for c, e in error_conns:
                print(f"    Errors @ c={c}: {e} total")


if __name__ == "__main__":
    main()
