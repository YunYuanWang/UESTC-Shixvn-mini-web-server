#!/usr/bin/env python3
"""
parse_wrk_output.py — Parse wrk2 latency output files into CSV.

Usage:
    python3 parse_wrk_output.py <wrk_output_file>
    python3 parse_wrk_output.py --dir <directory_of_wrk_files> --output <output.csv>
"""

import re
import sys
import os
import csv
import argparse
from pathlib import Path


def parse_wrk2_file(filepath):
    """Parse a single wrk2 output file. Returns dict of metrics or None."""
    try:
        with open(filepath, 'r') as f:
            content = f.read()
    except (IOError, OSError) as e:
        print(f"WARNING: Cannot read {filepath}: {e}", file=sys.stderr)
        return None

    if not content.strip():
        return None

    result = {
        'qps': 0.0,
        'avg_latency_ms': 0.0,
        'max_latency_ms': 0.0,
        'stdev_ms': 0.0,
        'p50_ms': 0.0,
        'p75_ms': 0.0,
        'p90_ms': 0.0,
        'p95_ms': 0.0,
        'p99_ms': 0.0,
        'p99.9_ms': 0.0,
        'p100_ms': 0.0,
        'total_requests': 0,
        'duration_s': 0.0,
        'socket_errors': 0,
        'timeouts': 0,
    }

    # Requests/sec: 50790.40
    m = re.search(r'Requests/sec:\s+([\d.]+)', content)
    if m:
        result['qps'] = float(m.group(1))

    # Total requests
    m = re.search(r'(\d+)\s+requests in\s+([\d.]+)s', content)
    if m:
        result['total_requests'] = int(m.group(1))
        result['duration_s'] = float(m.group(2))

    # Thread Stats Latency line: "Latency     1.23ms   0.56ms  12.34ms    75.00%"
    m = re.search(r'Latency\s+([\d.]+)(ms|us|s)\s+([\d.]+)(ms|us|s)\s+([\d.]+)(ms|us|s)', content)
    if m:
        avg_val = float(m.group(1))
        avg_unit = m.group(2)
        stdev_val = float(m.group(3))
        stdev_unit = m.group(4)
        max_val = float(m.group(5))
        max_unit = m.group(6)

        result['avg_latency_ms'] = _to_ms(avg_val, avg_unit)
        result['stdev_ms'] = _to_ms(stdev_val, stdev_unit)
        result['max_latency_ms'] = _to_ms(max_val, max_unit)

    # Latency Distribution percentiles
    percentile_map = {
        '50.000': 'p50_ms',
        '75.000': 'p75_ms',
        '90.000': 'p90_ms',
        '95.000': 'p95_ms',
        '99.000': 'p99_ms',
        '99.900': 'p99.9_ms',
        '100.000': 'p100_ms',
    }

    for line in content.split('\n'):
        # Lines like: " 50.000%    1.23ms"
        m = re.match(r'\s*([\d.]+)%\s+([\d.]+)(ms|us|s)', line)
        if m:
            pct = m.group(1)
            val = float(m.group(2))
            unit = m.group(3)
            if pct in percentile_map:
                result[percentile_map[pct]] = _to_ms(val, unit)

    # Socket errors
    m = re.search(r'Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)', content)
    if m:
        result['socket_errors'] = sum(int(x) for x in m.groups())

    # non-2xx responses
    m = re.search(r'Non-2xx or 3xx responses: (\d+)', content)
    if m:
        result['non_2xx'] = int(m.group(1))

    return result


def _to_ms(value, unit):
    """Convert latency value to milliseconds."""
    if unit == 's':
        return value * 1000.0
    elif unit == 'us':
        return value / 1000.0
    else:  # ms
        return value


def parse_directory(directory, output_csv):
    """Parse all wrk_*.log files in directory, write CSV."""
    rows = []
    pattern = re.compile(r'wrk_(\w+)_c(\d+)_r(\d+)\.log')

    for f in sorted(Path(directory).glob('wrk_*.log')):
        m = pattern.match(f.name)
        if not m:
            print(f"WARNING: filename {f.name} does not match expected pattern", file=sys.stderr)
            continue

        mode = m.group(1)
        concurrency = int(m.group(2))
        run = int(m.group(3))

        metrics = parse_wrk2_file(str(f))
        if metrics is None:
            continue

        rows.append({
            'mode': mode,
            'concurrency': concurrency,
            'run': run,
            **metrics,
        })

    if not rows:
        print("No valid wrk2 output files found.", file=sys.stderr)
        return

    # Sort by mode, concurrency, run
    rows.sort(key=lambda r: (r['mode'], r['concurrency'], r['run']))

    fieldnames = [
        'mode', 'concurrency', 'run', 'qps', 'avg_latency_ms', 'max_latency_ms',
        'stdev_ms', 'p50_ms', 'p75_ms', 'p90_ms', 'p95_ms', 'p99_ms',
        'p99.9_ms', 'p100_ms', 'total_requests', 'duration_s',
        'socket_errors', 'non_2xx',
    ]

    with open(output_csv, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)

    print(f"Parsed {len(rows)} results → {output_csv}")


def main():
    parser = argparse.ArgumentParser(description='Parse wrk2 output files to CSV')
    parser.add_argument('file', nargs='?', help='Single wrk2 output file to parse (prints JSON-like)')
    parser.add_argument('--dir', help='Directory containing wrk_*.log files')
    parser.add_argument('--output', '-o', default='parsed_results.csv', help='Output CSV file')
    args = parser.parse_args()

    if args.dir:
        parse_directory(args.dir, args.output)
    elif args.file:
        result = parse_wrk2_file(args.file)
        if result:
            for k, v in sorted(result.items()):
                print(f"{k}: {v}")
        else:
            print("Failed to parse file.", file=sys.stderr)
            sys.exit(1)
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
