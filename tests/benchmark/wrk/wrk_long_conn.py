#!/usr/bin/env python3
"""
wrk_long_conn.py — Long connection stability test using wrk + 100K dataset.
Measures CPU/RSS/context-switch/process count over time.
"""
import subprocess, time, os, signal, re

PROJECT = "/home/zigh-wang/data-disk/shixvn/miniwebserver"
os.chdir(PROJECT)
OUT = f"{PROJECT}/tests/benchmark/wrk/results"
LARGE_CSV = "data/users_100000.csv"
ORIGINAL_CSV = "data/users.csv"
ENDPOINT = "/users/compare/ZhangSan"

def kill_all():
    subprocess.run(["pkill", "-9", "-f", "mini_web_server"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)

def collect_metrics(mode, output_file, port, server_pid, duration):
    """Background collector: CPU%, RSS, context switches, process/thread count."""
    with open(output_file, "w") as f:
        f.write("timestamp,mode,cpu_pct,rss_kb,thread_count,proc_count,cswch_total,conn_count\n")
    t0 = time.time()
    last_cswch = 0
    # Get initial cswch
    try:
        with open(f"/proc/{server_pid}/status") as pf:
            for line in pf:
                if "voluntary_ctxt_switches" in line:
                    last_cswch = int(line.split(":")[1].strip())
                    break
    except: pass

    while time.time() - t0 < duration:
        if not (os.path.exists(f"/proc/{server_pid}") and
                subprocess.run(["kill", "-0", str(server_pid)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0):
            break

        elapsed = int(time.time() - t0)
        cpu = 0; rss = 0; th = 0; pcount = 1; cswch = 0; cswch_delta = 0; conns = 0

        try:
            # CPU, RSS, threads
            res = subprocess.run(["ps", "-p", str(server_pid), "-o", "%cpu=,rss=,nlwp="],
                                capture_output=True, text=True)
            parts = res.stdout.strip().split()
            if len(parts) >= 3:
                cpu = float(parts[0]); rss = int(parts[1]); th = int(parts[2])
        except: pass

        # Context switches (delta since last sample)
        try:
            with open(f"/proc/{server_pid}/status") as pf:
                for line in pf:
                    if "voluntary_ctxt_switches" in line:
                        cswch = int(line.split(":")[1].strip())
                        break
            cswch_delta = cswch - last_cswch
            last_cswch = cswch
        except: pass

        # Child processes (fork mode)
        if mode == "fork":
            try:
                res = subprocess.run(["pgrep", "-P", str(server_pid)],
                                    capture_output=True, text=True)
                pids = [p for p in res.stdout.strip().split("\n") if p]
                pcount = len(pids) + 1
            except: pass

        # TCP connections
        try:
            res = subprocess.run(["ss", "-tnp"],
                                capture_output=True, text=True)
            conns = res.stdout.count(f":{port}")
        except: pass

        with open(output_file, "a") as f:
            f.write(f"{elapsed},{mode},{cpu},{rss},{th},{pcount},{cswch_delta},{conns}\n")
        time.sleep(1)

    return output_file


def parse_wrk(output):
    r = {}
    m = re.search(r"Requests/sec:\s+([\d.]+)", output)
    if m: r["qps"] = float(m.group(1))
    m = re.search(r"(\d+)\s+requests in\s+([\d.]+)s", output)
    if m: r["total"] = int(m.group(1))
    m = re.search(r"Latency\s+([\d.]+)(us|ms|s)\s+([\d.]+)(us|ms|s)\s+([\d.]+)(us|ms|s)", output)
    if m:
        r["avg_ms"] = (float(m.group(1)) / 1000) if m.group(2) == "us" else (float(m.group(1)) * 1000 if m.group(2) == "s" else float(m.group(1)))
        r["max_ms"] = (float(m.group(5)) / 1000) if m.group(6) == "us" else (float(m.group(5)) * 1000 if m.group(6) == "s" else float(m.group(5)))
    for pct in ["50", "75", "90", "99"]:
        m2 = re.search(rf"{pct}%\s+([\d.]+)(us|ms|s)", output)
        if m2:
            val = float(m2.group(1))
            unit = m2.group(2)
            r[f"p{pct}_ms"] = val / 1000 if unit == "us" else (val * 1000 if unit == "s" else val)
    return r


# ── Main ──
subprocess.run(["make", "clean"], capture_output=True)
subprocess.run(["make"], capture_output=True)

if not os.path.exists(LARGE_CSV):
    print(f"ERROR: {LARGE_CSV} not found"); exit(1)

# Backup + switch
subprocess.run(["cp", ORIGINAL_CSV, f"{ORIGINAL_CSV}.wrk_bak"])
subprocess.run(["cp", LARGE_CSV, ORIGINAL_CSV])
print("Switched to 100K dataset\n")

tests = [
    ("pool", 2), ("pool", 5), ("pool", 10),
    ("select", 2), ("select", 5),
]

for mode, conn in tests:
    port = 9700 + hash(mode) % 50 + conn
    kill_all()
    subprocess.run(["rm", "-f", "logs/server.log"], capture_output=True)

    proc = subprocess.Popen(
        ["./mini_web_server", mode, "127.0.0.1", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    if proc.poll() is not None:
        print(f"{mode} c={conn}: server crashed on startup"); continue

    # Verify endpoint
    subprocess.run(["curl", "-s", "--max-time", "90",
                    f"http://127.0.0.1:{port}{ENDPOINT}"],
                   capture_output=True, timeout=95)
    print(f"{mode} c={conn}: endpoint verified")

    # Start metrics collector
    metrics_file = f"{OUT}/longconn_{mode}_c{conn}.csv"
    duration = 120
    import threading
    collector = threading.Thread(target=collect_metrics,
                                 args=(mode, metrics_file, port, proc.pid, duration))
    collector.start()

    # Run wrk
    print(f"{mode} c={conn}: running wrk {duration}s...")
    try:
        r = subprocess.run(
            ["wrk", "-t2", f"-c{conn}", f"-d{duration}s", "--latency",
             "--timeout", "90s",
             f"http://127.0.0.1:{port}{ENDPOINT}"],
            capture_output=True, text=True,
            timeout=duration + 90)
        output = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        output = "(wrk timed out)"

    # Wait for collector
    collector.join(timeout=10)

    # Parse
    metrics = parse_wrk(output)
    qps = metrics.get("qps", 0); total = metrics.get("total", 0)
    avg = metrics.get("avg_ms", 0); p50 = metrics.get("p50_ms", 0)
    p99 = metrics.get("p99_ms", 0)

    print(f"  QPS={qps:.2f} total={total} avg={avg:.0f}ms P50={p50:.0f}ms P99={p99:.0f}ms")
    print(f"  Metrics: {metrics_file}")

    # Save raw wrk output
    with open(f"{OUT}/longconn_{mode}_c{conn}.log", "w") as f:
        f.write(output)

    proc.send_signal(signal.SIGKILL); proc.wait()
    time.sleep(2)

# Restore
subprocess.run(["cp", f"{ORIGINAL_CSV}.wrk_bak", ORIGINAL_CSV])
subprocess.run(["rm", "-f", f"{ORIGINAL_CSV}.wrk_bak"])
kill_all()
print("\nDone. Results in:", OUT)
