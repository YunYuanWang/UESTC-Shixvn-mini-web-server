#!/usr/bin/env python3
"""wrk2 benchmark harness — test all 4 modes at various concurrency levels."""
import subprocess, time, os, signal, json, re, sys

PROJECT = "/home/zigh-wang/data-disk/shixvn/miniwebserver"
os.chdir(PROJECT)

def kill_all():
    subprocess.run(["pkill", "-9", "-f", "mini_web_server"], capture_output=True)
    time.sleep(0.3)

def build():
    subprocess.run(["make", "clean"], capture_output=True)
    subprocess.run(["make"], capture_output=True)

def _to_ms(val, unit):
    if unit == 'us': return val / 1000
    if unit == 's': return val * 1000
    return val

def run_test(mode, port, conn, duration=10, rate=500000):
    """Run one wrk2 test. Returns dict of metrics."""
    kill_all()
    subprocess.run(["rm", "-f", "logs/server.log"])

    # Start server
    proc = subprocess.Popen(
        ["./mini_web_server", mode, "127.0.0.1", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)

    if proc.poll() is not None:
        return {"error": "server_crashed"}

    # Warmup
    subprocess.run(["curl", "-s", "--max-time", "2",
                    f"http://127.0.0.1:{port}/hello"],
                   capture_output=True, timeout=3)

    # wrk2
    threads = min(4, os.cpu_count() or 2)
    try:
        r = subprocess.run(
            ["wrk2", f"-t{threads}", f"-c{conn}", f"-d{duration}s",
             f"-R{rate}", "--latency", f"http://127.0.0.1:{port}/hello"],
            capture_output=True, text=True, timeout=duration + 20)
        output = r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGKILL)
        proc.wait()
        return {"error": "wrk2_timeout"}

    proc.send_signal(signal.SIGKILL)
    proc.wait()
    time.sleep(0.5)

    # Parse wrk2 output
    result = {}

    # QPS
    m = re.search(r'Requests/sec:\s+([\d.]+)', output)
    if m: result['qps'] = float(m.group(1))

    # Total requests
    m = re.search(r'(\d+)\s+requests in\s+([\d.]+)s', output)
    if m:
        result['total'] = int(m.group(1))
        result['duration'] = float(m.group(2))

    # Latency stats: "    Latency   108.66ms  128.78ms 505.60ms   81.32%"
    m = re.search(r'Latency\s+([\d.]+)(ms|us|s)\s+([\d.]+)(ms|us|s)\s+([\d.]+)(ms|us|s)', output)
    if m:
        result['avg_ms'] = _to_ms(float(m.group(1)), m.group(2))
        result['max_ms'] = _to_ms(float(m.group(5)), m.group(6))

    # Percentiles: " 50.000%   49.38ms"
    for pct in ['50.000', '75.000', '90.000', '95.000', '99.000']:
        m = re.search(r'%s%%\s+([\d.]+)(ms|us|s)' % pct.replace('.', '\\.'), output)
        if m:
            val = _to_ms(float(m.group(1)), m.group(2))
            key = 'p%s_ms' % pct.split('.')[0]
            result[key] = val

    return result


def main():
    build()

    # Test configurations: mode -> [(conn, rate), ...]
    tests = {
        'select': [(10, 50000), (50, 200000), (100, 500000), (200, 1000000),
                    (500, 2000000), (800, 2000000), (1000, 2000000), (1100, 2000000)],
        'pool':   [(10, 50000), (50, 200000), (100, 500000), (200, 1000000),
                    (500, 2000000), (800, 2000000), (1000, 2000000)],
        'thread': [(10, 50000), (50, 200000), (100, 500000), (200, 1000000),
                    (500, 2000000), (800, 2000000), (1000, 2000000)],
        'fork':   [(10, 50000), (50, 200000), (100, 500000), (200, 1000000),
                    (500, 2000000)],
    }

    print(f"{'mode':<8} {'conn':>5} {'QPS':>8} {'avg_ms':>8} {'P50':>8} {'P95':>8} {'P99':>8} {'max_ms':>8} {'total':>8}")
    print("-" * 80)

    csv_lines = ["mode,concurrency,qps,avg_ms,p50_ms,p75_ms,p90_ms,p95_ms,p99_ms,max_ms,total"]

    for mode in ['thread', 'pool', 'select', 'fork']:
        for conn, rate in tests[mode]:
            port = 9500 + (conn % 100) * 10 + hash(mode) % 10
            result = run_test(mode, port, conn, duration=10, rate=rate)

            if 'error' in result:
                print(f"{mode:<8} {conn:>5}  ERROR: {result['error']}")
                csv_lines.append(f"{mode},{conn},0,0,0,0,0,0,0,0,error:{result['error']}")
            else:
                qps = result.get('qps', 0)
                avg = result.get('avg_ms', 0)
                p50 = result.get('p50_ms', 0)
                p95 = result.get('p95_ms', 0)
                p99 = result.get('p99_ms', 0)
                mx  = result.get('max_ms', 0)
                tot = result.get('total', 0)
                print(f"{mode:<8} {conn:>5} {qps:>8.0f} {avg:>8.2f} {p50:>8.2f} {p95:>8.2f} {p99:>8.2f} {mx:>8.2f} {tot:>8}")
                csv_lines.append(f"{mode},{conn},{qps},{avg},{p50},0,0,{p95},{p99},{mx},{tot}")

        print()

    # Save CSV
    csv_path = f"{PROJECT}/tests/benchmark/results/wrk2/final_results.csv"
    with open(csv_path, 'w') as f:
        f.write('\n'.join(csv_lines))
    print(f"Results saved to: {csv_path}")

if __name__ == '__main__':
    main()
