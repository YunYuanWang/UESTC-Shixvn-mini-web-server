#!/usr/bin/env python3
"""
http_bench.py — Simple HTTP benchmarker for mini_web_server testing.

Uses only the standard library (socket + time + threading).
Avoids ab/wrk2 compatibility issues with non-keepalive servers.

Usage:
    python3 http_bench.py --url http://127.0.0.1:8080/hello -c 10 -d 5
"""

import socket
import time
import threading
import argparse
import sys
from collections import defaultdict


def bench_worker(url, host, port, path, duration, results, stop_event, worker_id, req_timeout=1.0):
    """Single worker thread: connect, send request, read response, repeat."""
    count = 0
    errors = 0
    latencies = []
    start_time = time.time()

    while not stop_event.is_set():
        sock = None
        try:
            t0 = time.time()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(req_timeout)  # configurable timeout
            sock.connect((host, port))

            # Send minimal HTTP/1.0 request (no keep-alive)
            request = f"GET {path} HTTP/1.0\r\nHost: {host}\r\n\r\n"
            sock.sendall(request.encode())

            # Read response — server sends full response then closes connection
            # Use a single recv() with timeout; the response fits in one TCP segment
            response = sock.recv(4096)
            t1 = time.time()

            # Validate: must be HTTP 200
            if response and b'200 OK' in response:
                latencies.append((t1 - t0) * 1000)  # ms
                count += 1
            else:
                errors += 1

        except socket.timeout:
            errors += 1  # server didn't respond in time
        except Exception as e:
            errors += 1
        finally:
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass

    results[worker_id] = {
        'count': count,
        'errors': errors,
        'latencies': latencies,
    }


def run_benchmark(url, concurrency, duration, req_timeout=1.0):
    """Run benchmark with specified concurrency and duration."""
    # Parse URL
    from urllib.parse import urlparse
    parsed = urlparse(url)
    host = parsed.hostname or '127.0.0.1'
    port = parsed.port or 8080
    path = parsed.path or '/hello'

    print(f"Benchmark: {url}")
    print(f"  Concurrency: {concurrency}")
    print(f"  Duration:    {duration}s")
    print(f"  Target:      {host}:{port}{path}")
    print()

    stop_event = threading.Event()
    results = {}
    threads = []

    # Warmup
    print("Warmup...")
    warm_results = {}
    warm_stop = threading.Event()
    warm_threads = []
    for i in range(min(2, concurrency)):
        t = threading.Thread(target=bench_worker,
                             args=(url, host, port, path, 1, warm_results,
                                   warm_stop, f"warm_{i}", req_timeout))
        warm_threads.append(t)
        t.start()
    time.sleep(0.5)
    warm_stop.set()
    for t in warm_threads:
        t.join(timeout=2)
    print("Warmup complete.\n")

    # Main benchmark
    print(f"Running {duration}s benchmark with {concurrency} concurrent connections...")
    start_time = time.time()

    for i in range(concurrency):
        t = threading.Thread(target=bench_worker,
                             args=(url, host, port, path, duration, results,
                                   stop_event, f"worker_{i}", req_timeout),
                             daemon=True)
        threads.append(t)
        t.start()

    # Wait for duration
    time.sleep(duration)
    stop_event.set()

    # Wait for all threads (with timeout)
    deadline = time.time() + 10
    for t in threads:
        remaining = deadline - time.time()
        if remaining > 0:
            t.join(timeout=remaining)

    elapsed = time.time() - start_time

    # Aggregate results
    total_requests = sum(r['count'] for r in results.values())
    total_errors = sum(r['errors'] for r in results.values())
    all_latencies = []
    for r in results.values():
        all_latencies.extend(r['latencies'])

    all_latencies.sort()
    n = len(all_latencies)

    print(f"\n{'='*60}")
    print(f"Results ({elapsed:.1f}s elapsed)")
    print(f"{'='*60}")
    print(f"  Total requests:   {total_requests}")
    print(f"  Total errors:     {total_errors}")
    print(f"  Requests/sec:     {total_requests / elapsed:.1f}")
    print()

    if all_latencies:
        avg = sum(all_latencies) / n
        print(f"  Avg latency:      {avg:.2f} ms")
        print(f"  Min latency:      {all_latencies[0]:.2f} ms")
        print(f"  Max latency:      {all_latencies[-1]:.2f} ms")
        print(f"  P50 latency:      {all_latencies[n // 2]:.2f} ms")
        print(f"  P75 latency:      {all_latencies[n * 3 // 4]:.2f} ms")
        print(f"  P90 latency:      {all_latencies[n * 9 // 10]:.2f} ms")
        print(f"  P95 latency:      {all_latencies[n * 95 // 100]:.2f} ms")
        print(f"  P99 latency:      {all_latencies[n * 99 // 100]:.2f} ms")
    else:
        print("  (no successful requests)")
        return None

    print(f"{'='*60}")

    return {
        'total_requests': total_requests,
        'total_errors': total_errors,
        'qps': total_requests / elapsed if elapsed > 0 else 0,
        'avg_latency_ms': avg,
        'max_latency_ms': all_latencies[-1] if all_latencies else 0,
        'p50_ms': all_latencies[n // 2] if all_latencies else 0,
        'p75_ms': all_latencies[n * 3 // 4] if all_latencies else 0,
        'p90_ms': all_latencies[n * 9 // 10] if all_latencies else 0,
        'p95_ms': all_latencies[n * 95 // 100] if all_latencies else 0,
        'p99_ms': all_latencies[n * 99 // 100] if all_latencies else 0,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='HTTP Benchmark Tool')
    parser.add_argument('--url', required=True, help='Target URL')
    parser.add_argument('-c', '--concurrency', type=int, default=10,
                        help='Concurrent connections (default: 10)')
    parser.add_argument('-d', '--duration', type=int, default=5,
                        help='Test duration in seconds (default: 5)')
    parser.add_argument('--timeout', type=float, default=1.0,
                        help='Socket timeout in seconds (default: 1.0)')
    args = parser.parse_args()

    run_benchmark(args.url, args.concurrency, args.duration, args.timeout)
