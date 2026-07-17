#!/usr/bin/env python3
"""
wrk_bench.py — Pure wrk benchmark for all 4 server modes.
Finds performance limits: QPS ceiling, latency knee, connection errors.
"""
import subprocess, time, os, signal, re, sys

PROJECT = "/home/zigh-wang/data-disk/shixvn/miniwebserver"
os.chdir(PROJECT)
OUT = f"{PROJECT}/tests/benchmark/wrk/results"

def kill_all():
    subprocess.run(["pkill", "-9", "-f", "mini_web_server"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)

def _to_ms(val, unit):
    if unit == "us": return val / 1000.0
    if unit == "s": return val * 1000.0
    return float(val)

def parse_wrk(output):
    """Parse wrk output. Returns dict."""
    r = {}
    # Requests/sec:    183.07
    m = re.search(r"Requests/sec:\s+([\d.]+)", output)
    if m: r["qps"] = float(m.group(1))
    # N requests in Ds
    m = re.search(r"(\d+)\s+requests in\s+([\d.]+)s", output)
    if m: r["total"] = int(m.group(1))
    # Latency   137.45us  236.83us   4.11ms   95.07%
    m = re.search(r"Latency\s+([\d.]+)(us|ms|s)\s+([\d.]+)(us|ms|s)\s+([\d.]+)(us|ms|s)", output)
    if m:
        r["avg_ms"] = _to_ms(float(m.group(1)), m.group(2))
        r["stdev_ms"] = _to_ms(float(m.group(3)), m.group(4))
        r["max_ms"] = _to_ms(float(m.group(5)), m.group(6))
    # Req/Sec    27.50k   707.11    28.00k   100.00%
    m = re.search(r"Req/Sec\s+([\d.]+)(k|M|)\s+", output)
    if m:
        val = float(m.group(1))
        if m.group(2) == "k": val *= 1000
        elif m.group(2) == "M": val *= 1000000
        r["req_per_sec"] = val
    # Percentiles: "     50%   80.00us"
    for pct in ["50", "75", "90", "99"]:
        m = re.search(rf"{pct}%\s+([\d.]+)(us|ms|s)", output)
        if m:
            r[f"p{pct}_ms"] = _to_ms(float(m.group(1)), m.group(2))
    # Socket errors
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", output)
    if m:
        r["err_connect"] = int(m.group(1))
        r["err_read"] = int(m.group(2))
        r["err_write"] = int(m.group(3))
        r["err_timeout"] = int(m.group(4))
    return r

def run_test(mode, port, conn):
    """Run wrk against one mode. Returns (metrics, raw_output)."""
    kill_all()
    subprocess.run(["rm", "-f", "logs/server.log"], capture_output=True)

    proc = subprocess.Popen(
        ["./mini_web_server", mode, "127.0.0.1", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    if proc.poll() is not None:
        return {"error": "server_crashed"}, ""

    duration = 15 if conn <= 200 else 20
    wrk_timeout = 2 if conn <= 200 else 5

    try:
        r = subprocess.run(
            ["wrk", "-t4", f"-c{conn}", f"-d{duration}s", "--latency",
             f"--timeout", f"{wrk_timeout}s",
             f"http://127.0.0.1:{port}/hello"],
            capture_output=True, text=True,
            timeout=duration + 60)
        output = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGKILL); proc.wait()
        return {"error": "wrk_timeout"}, ""

    proc.send_signal(signal.SIGKILL); proc.wait()
    time.sleep(0.3)

    metrics = parse_wrk(output)
    metrics["conn"] = conn; metrics["duration"] = duration
    return metrics, output


# ── Test matrix ──
configs = {
    "epoll":  [10, 50, 100, 200, 500, 800, 1000, 1024, 1100, 1500, 2000, 3000],
    "select": [10, 50, 100, 200, 500, 800, 1000, 1024, 1100, 1500],
    "pool":   [10, 50, 100, 200, 500, 1000, 2000, 3000],
    "thread": [10, 50, 100, 200, 500, 1000, 1500, 2000],
    "fork":   [1, 5, 10, 20, 50, 100, 200, 500, 1000],
}

subprocess.run(["make", "clean"], capture_output=True)
subprocess.run(["make"], capture_output=True)

all_csv = f"{OUT}/all_results.csv"
with open(all_csv, "w") as f:
    f.write("mode,concurrency,qps,avg_ms,p50_ms,p75_ms,p90_ms,p99_ms,max_ms,req_per_sec,err_connect,err_read,err_write,err_timeout,total,duration_s\n")

for mode in ["epoll", "select", "pool", "thread", "fork"]:
    os.makedirs(f"{OUT}/logs/{mode}", exist_ok=True)
    levels = configs[mode]
    print(f"\n{'='*85}")
    print(f"  {mode.upper()}")
    print(f"{'='*85}")
    print(f"{'conn':>6} {'QPS':>9} {'avg':>8} {'P50':>8} {'P90':>8} {'P99':>8} {'max':>8}  "
          f"{'err_c':>5} {'err_r':>5} {'err_t':>5} {'total':>8}")
    print("-" * 85)

    for conn in levels:
        port = 9600 + hash(mode) % 50 + conn % 50
        print(f"\r{mode} c={conn}: running...", end="", flush=True)
        metrics, raw = run_test(mode, port, conn)

        # Save raw wrk output
        with open(f"{OUT}/logs/{mode}/c{conn}.log", "w") as f:
            f.write(raw)

        if "error" in metrics:
            print(f"\r{mode} c={conn}: ERROR {metrics['error']}")
            with open(all_csv, "a") as f:
                f.write(f"{mode},{conn},0,0,0,0,0,0,0,0,0,0,0,0,0,0\n")
            # On crash, stop testing this mode
            if metrics["error"] == "server_crashed":
                break
            continue

        qps = metrics.get("qps", 0); avg = metrics.get("avg_ms", 0)
        p50 = metrics.get("p50_ms", 0); p90 = metrics.get("p90_ms", 0)
        p99 = metrics.get("p99_ms", 0); mx = metrics.get("max_ms", 0)
        rps = metrics.get("req_per_sec", 0)
        ec = metrics.get("err_connect", 0); er = metrics.get("err_read", 0)
        et = metrics.get("err_timeout", 0); total = metrics.get("total", 0)

        print(f"\r{conn:>6} {qps:>9.1f} {avg:>8.2f} {p50:>8.2f} {p90:>8.2f} {p99:>8.2f} {mx:>8.2f}  "
              f"{ec:>5} {er:>5} {et:>5} {total:>8}")

        with open(all_csv, "a") as f:
            f.write(f"{mode},{conn},{qps},{avg},{p50},"
                    f"{metrics.get('p75_ms',0)},{p90},{p99},{mx},{rps},"
                    f"{ec},{er},{metrics.get('err_write',0)},{et},{total},{metrics.get('duration',0)}\n")

        time.sleep(1)

    print(f"--- {mode} complete ---")

kill_all()
print(f"\nResults: {all_csv}")
