#!/usr/bin/env python3
"""
plot_results.py — Generate comparison charts for mini_web_server benchmark.

Reads CSV data from performance tests (latency.csv) and long-connection
stability tests (stability.csv), produces publication-quality charts.

Usage:
    python3 plot_results.py --perf results/perf/latency.csv
    python3 plot_results.py --stability results/long_conn/stability.csv
    python3 plot_results.py --perf ... --stability ... --all
    python3 plot_results.py --perf ... --stability ... --output results/charts/
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np
import csv
import argparse
import os
import sys
from pathlib import Path
from collections import defaultdict

# ── Design constants ──────────────────────────────────────────────
MODES = ['fork', 'thread', 'pool', 'select']
MODE_LABELS = {'fork': 'Fork (Multi-Process)',
               'thread': 'Thread (Per-Connection)',
               'pool': 'Pool (Thread Pool)',
               'select': 'Select (I/O Multiplexing)'}
MODE_COLORS = {'fork': '#e74c3c', 'thread': '#f39c12',
               'pool': '#3498db', 'select': '#2ecc71'}
MODE_MARKERS = {'fork': 'o', 'thread': 'D',
                'pool': 's', 'select': '^'}

# Surface and ink (light mode)
SURFACE = '#fcfcfb'
INK_PRIMARY = '#0b0b0b'
INK_SECONDARY = '#52514e'
INK_MUTED = '#898781'
GRIDLINE = '#e1e0d9'
BASELINE = '#c3c2b7'

DPI = 150
FIGSIZE_WIDE = (12, 6)
FIGSIZE_SQUARE = (8, 8)
FIGSIZE_DASHBOARD = (14, 10)


def setup_style():
    """Apply global matplotlib style matching the design system."""
    plt.rcParams.update({
        'figure.facecolor': SURFACE,
        'axes.facecolor': SURFACE,
        'axes.edgecolor': BASELINE,
        'axes.linewidth': 1.0,
        'axes.grid': True,
        'grid.color': GRIDLINE,
        'grid.linewidth': 0.5,
        'grid.alpha': 1.0,
        'axes.titlesize': 13,
        'axes.titleweight': 'semibold',
        'axes.labelsize': 11,
        'axes.labelcolor': INK_SECONDARY,
        'xtick.color': INK_MUTED,
        'ytick.color': INK_MUTED,
        'text.color': INK_PRIMARY,
        'legend.facecolor': SURFACE,
        'legend.edgecolor': GRIDLINE,
        'legend.fontsize': 9,
        'legend.title_fontsize': 10,
        'legend.framealpha': 0.9,
        'font.family': 'sans-serif',
        'font.size': 10,
        'lines.linewidth': 2,
        'lines.markersize': 7,
        'lines.markeredgewidth': 2,
        'lines.markeredgecolor': SURFACE,
    })


# ── Data loading ──────────────────────────────────────────────────

def load_perf_data(csv_path):
    """Load and aggregate performance CSV. Returns list of dicts or None."""
    if not os.path.exists(csv_path):
        print(f"WARNING: {csv_path} not found — skipping performance charts")
        return None
    rows = []
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    if not rows:
        return None

    # Group by (mode, concurrency) and aggregate across runs
    groups = defaultdict(list)
    for r in rows:
        key = (r['mode'], int(r['concurrency']))
        groups[key].append(r)

    aggregated = []
    for (mode, conn), group in sorted(groups.items()):
        def median_or_zero(values):
            s = sorted(v for v in values if v is not None)
            if not s:
                return 0.0
            n = len(s)
            return s[n // 2] if n % 2 else (s[n//2-1] + s[n//2]) / 2.0

        def mean_or_zero(values):
            s = [v for v in values if v is not None]
            return sum(s) / len(s) if s else 0.0

        row = {
            'mode': mode,
            'concurrency': conn,
            'qps': median_or_zero([float(r.get('qps', 0) or 0) for r in group]),
            'avg_latency_ms': median_or_zero([float(r.get('avg_latency_ms', 0) or 0) for r in group]),
            'max_latency_ms': median_or_zero([float(r.get('max_latency_ms', 0) or 0) for r in group]),
            'p50_ms': median_or_zero([float(r.get('p50_ms', 0) or 0) for r in group]),
            'p75_ms': median_or_zero([float(r.get('p75_ms', 0) or 0) for r in group]),
            'p90_ms': median_or_zero([float(r.get('p90_ms', 0) or 0) for r in group]),
            'p95_ms': median_or_zero([float(r.get('p95_ms', 0) or 0) for r in group]),
            'p99_ms': median_or_zero([float(r.get('p99_ms', 0) or 0) for r in group]),
            'p99_9_ms': median_or_zero([float(r.get('p99.9_ms', 0) or 0) for r in group]),
            'cpu_avg_pct': mean_or_zero([float(r.get('cpu_avg_pct', 0) or 0) for r in group]),
            'rss_avg_kb': mean_or_zero([float(r.get('rss_avg_kb', 0) or 0) for r in group]),
            'cswch_avg_per_sec': mean_or_zero([float(r.get('cswch_avg_per_sec', 0) or 0) for r in group]),
            'total_requests': median_or_zero([float(r.get('total_requests', 0) or 0) for r in group]),
            'socket_errors': sum(int(r.get('socket_errors', 0) or 0) for r in group),
            'timeout_errors': sum(int(r.get('timeout_errors', 0) or 0) for r in group),
        }
        aggregated.append(row)
    return aggregated


def load_stability_data(csv_path):
    """Load stability time-series CSV. Returns dict of mode→list-of-rows."""
    if not os.path.exists(csv_path):
        print(f"WARNING: {csv_path} not found — skipping stability charts")
        return None
    result = defaultdict(list)
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            mode = row.get('mode', '')
            result[mode].append(row)
    if not result:
        return None
    return dict(result)


# ── Plotting helpers ───────────────────────────────────────────────

def add_legend(ax, title=None, loc='best'):
    """Add a legend in the design-system style (only if labeled artists exist)."""
    handles, labels = ax.get_legend_handles_labels()
    if not handles:
        return None
    # Filter out unlabeled artists
    valid = [(h, l) for h, l in zip(handles, labels) if l and not l.startswith('_')]
    if not valid:
        return None
    legend = ax.legend(loc=loc, frameon=True)
    if title:
        legend.set_title(title)
    return legend


def style_axes(ax, xlabel=None, ylabel=None, title=None):
    """Apply consistent axis styling."""
    if title:
        ax.set_title(title, fontweight='semibold', pad=12)
    if xlabel:
        ax.set_xlabel(xlabel, color=INK_SECONDARY)
    if ylabel:
        ax.set_ylabel(ylabel, color=INK_SECONDARY)
    ax.tick_params(colors=INK_MUTED)
    # Lighten spines
    for spine in ax.spines.values():
        spine.set_color(BASELINE)
        spine.set_linewidth(1.0)


def save_figure(fig, name, output_dir):
    """Save figure with consistent settings."""
    path = os.path.join(output_dir, name)
    fig.savefig(path, dpi=DPI, bbox_inches='tight', facecolor=SURFACE,
                edgecolor='none')
    plt.close(fig)
    print(f"  Saved: {path}")


def line_for_mode(ax, data, mode, x_key, y_key, marker=True):
    """Plot a clean line for one server mode from list-of-dicts data."""
    subset = sorted(
        [r for r in data if r['mode'] == mode and r.get(y_key, 0) is not None],
        key=lambda r: r[x_key])
    if not subset:
        return
    x = [r[x_key] for r in subset]
    y = [float(r[y_key]) if r[y_key] else 0.0 for r in subset]
    if all(v <= 0 for v in y):
        return
    color = MODE_COLORS[mode]
    kwargs = dict(color=color, linewidth=2, zorder=3, label=MODE_LABELS[mode])
    if marker and len(x) > 1:
        kwargs['marker'] = MODE_MARKERS[mode]
        kwargs['markersize'] = 8
        kwargs['markerfacecolor'] = color
        kwargs['markeredgecolor'] = SURFACE
        kwargs['markeredgewidth'] = 2
    ax.plot(x, y, **kwargs)
    # endpoint label
    if len(y) > 0 and y[-1] > 0:
        val_str = f'{y[-1]:.1f}' if isinstance(y[-1], float) else str(y[-1])
        ax.annotate(val_str, xy=(x[-1], y[-1]),
                    xytext=(6, 2), textcoords='offset points',
                    fontsize=8, color=INK_SECONDARY, va='bottom')


# ── Scenario A: Performance Charts ─────────────────────────────────

def get_sorted_levels(data):
    """Get sorted unique concurrency levels from aggregated data."""
    return sorted(set(r['concurrency'] for r in data))


def get_mode_row(data, mode, conn_level):
    """Get the aggregated row for a mode at a specific concurrency level."""
    for r in data:
        if r['mode'] == mode and r['concurrency'] == conn_level:
            return r
    return None


def plot_concurrency_vs_latency(data, output_dir):
    """Chart 1: Concurrency vs Average Latency (ms)."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        line_for_mode(ax, data, mode, 'concurrency', 'avg_latency_ms')
    levels = get_sorted_levels(data)
    style_axes(ax, xlabel='Concurrent Connections',
               ylabel='Average Latency (ms)',
               title='Concurrency vs Average Response Time')
    add_legend(ax)
    ax.set_xticks(levels)
    save_figure(fig, '01_avg_latency.png', output_dir)


def plot_concurrency_vs_p95(data, output_dir):
    """Chart 2: Concurrency vs P95 Latency (ms)."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        line_for_mode(ax, data, mode, 'concurrency', 'p95_ms')
    levels = get_sorted_levels(data)
    style_axes(ax, xlabel='Concurrent Connections',
               ylabel='P95 Latency (ms)',
               title='Concurrency vs P95 Response Time')
    add_legend(ax)
    ax.set_xticks(levels)
    save_figure(fig, '02_p95_latency.png', output_dir)


def plot_concurrency_vs_qps(data, output_dir):
    """Chart 3: Concurrency vs QPS Throughput."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        line_for_mode(ax, data, mode, 'concurrency', 'qps')
    levels = get_sorted_levels(data)
    style_axes(ax, xlabel='Concurrent Connections',
               ylabel='Requests / Second (QPS)',
               title='Concurrency vs Throughput (QPS)')
    add_legend(ax)
    ax.set_xticks(levels)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(
        lambda x, _: f'{x/1000:.0f}K' if x >= 1000 else f'{x:.0f}'))
    save_figure(fig, '03_qps_throughput.png', output_dir)


def plot_concurrency_vs_cpu(data, output_dir):
    """Chart 4: Concurrency vs CPU Usage (grouped bar)."""
    # Check if CPU data exists
    has_cpu = any(float(r.get('cpu_avg_pct', 0) or 0) > 0 for r in data)
    if not has_cpu:
        print("  (no CPU data — skipping CPU chart)")
        return
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    levels = get_sorted_levels(data)
    n_levels = len(levels)
    n_modes = len(MODES)
    bar_width = min(20.0 / (n_modes + 1), 0.7 / n_modes)
    x = np.arange(n_levels)

    for i, mode in enumerate(MODES):
        values = []
        for lvl in levels:
            row = get_mode_row(data, mode, lvl)
            values.append(float(row['cpu_avg_pct']) if row else 0)
        offset = (i - n_modes / 2 + 0.5) * bar_width
        bars = ax.bar(x + offset, values, bar_width * 0.9,
                      color=MODE_COLORS[mode], label=MODE_LABELS[mode],
                      zorder=3, edgecolor=SURFACE, linewidth=1)
        for bar, val in zip(bars, values):
            if val > 0 and val >= max(values) * 0.8:
                ax.text(bar.get_x() + bar.get_width() / 2,
                        bar.get_height() + 1,
                        f'{val:.0f}%', ha='center', fontsize=8,
                        color=INK_SECONDARY)

    style_axes(ax, xlabel='Concurrent Connections',
               ylabel='CPU Usage (%)',
               title='Concurrency vs CPU Utilization')
    ax.set_xticks(x)
    ax.set_xticklabels(levels)
    add_legend(ax)
    save_figure(fig, '04_cpu_usage.png', output_dir)


def plot_concurrency_vs_memory(data, output_dir):
    """Chart 5: Concurrency vs Memory Usage (RSS MB)."""
    has_rss = any(float(r.get('rss_avg_kb', 0) or 0) > 0 for r in data)
    if not has_rss:
        print("  (no RSS data — skipping memory chart)")
        return
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    levels = get_sorted_levels(data)
    n_levels = len(levels)
    n_modes = len(MODES)
    bar_width = min(20.0 / (n_modes + 1), 0.7 / n_modes)
    x = np.arange(n_levels)

    for i, mode in enumerate(MODES):
        values = []
        for lvl in levels:
            row = get_mode_row(data, mode, lvl)
            val = float(row['rss_avg_kb']) if row else 0
            values.append(val / 1024.0)  # KB -> MB
        offset = (i - n_modes / 2 + 0.5) * bar_width
        ax.bar(x + offset, values, bar_width * 0.9,
               color=MODE_COLORS[mode], label=MODE_LABELS[mode],
               zorder=3, edgecolor=SURFACE, linewidth=1)

    style_axes(ax, xlabel='Concurrent Connections',
               ylabel='Memory RSS (MB)',
               title='Concurrency vs Memory Usage')
    ax.set_xticks(x)
    ax.set_xticklabels(levels)
    add_legend(ax)
    save_figure(fig, '05_memory_usage.png', output_dir)


def plot_perf_dashboard(data, output_dir):
    """Chart 6: 2x2 performance summary dashboard."""
    fig, axes = plt.subplots(2, 2, figsize=FIGSIZE_DASHBOARD)
    levels = get_sorted_levels(data)

    # Top-left: Avg latency
    ax = axes[0, 0]
    for mode in MODES:
        line_for_mode(ax, data, mode, 'concurrency', 'avg_latency_ms')
    style_axes(ax, xlabel='Connections', ylabel='Avg Latency (ms)')
    ax.set_title('Average Response Time', fontsize=11, fontweight='semibold')

    # Top-right: QPS
    ax = axes[0, 1]
    for mode in MODES:
        line_for_mode(ax, data, mode, 'concurrency', 'qps')
    style_axes(ax, xlabel='Connections', ylabel='QPS')
    ax.set_title('Throughput', fontsize=11, fontweight='semibold')
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(
        lambda x, _: f'{x/1000:.0f}K' if x >= 1000 else f'{x:.0f}'))

    # Bottom-left: P95 latency
    ax = axes[1, 0]
    for mode in MODES:
        line_for_mode(ax, data, mode, 'concurrency', 'p95_ms')
    style_axes(ax, xlabel='Connections', ylabel='P95 Latency (ms)')
    ax.set_title('P95 Response Time', fontsize=11, fontweight='semibold')

    # Bottom-right: CPU or QPS bars
    ax = axes[1, 1]
    has_cpu = any(float(r.get('cpu_avg_pct', 0) or 0) > 0 for r in data)
    n_modes = len(MODES)
    bar_width = min(20.0 / (n_modes + 1), 0.7 / n_modes)
    x = np.arange(len(levels))
    for i, mode in enumerate(MODES):
        values = []
        for lvl in levels:
            row = get_mode_row(data, mode, lvl)
            if has_cpu:
                values.append(float(row['cpu_avg_pct']) if row else 0)
            else:
                values.append(float(row['qps']) / 1000 if row else 0)  # QPS in K
        offset = (i - n_modes / 2 + 0.5) * bar_width
        ax.bar(x + offset, values, bar_width * 0.9,
               color=MODE_COLORS[mode], label=MODE_LABELS[mode],
               zorder=3, edgecolor=SURFACE, linewidth=1)
    style_axes(ax, xlabel='Connections',
               ylabel='CPU (%)' if has_cpu else 'QPS (K)')
    ax.set_title('CPU Utilization' if has_cpu else 'Throughput (QPS)', fontsize=11, fontweight='semibold')
    ax.set_xticks(x)
    ax.set_xticklabels(levels)

    # Shared legend
    handles = [plt.Line2D([0], [0], color=MODE_COLORS[m], linewidth=2,
                          marker=MODE_MARKERS[m], markersize=8,
                          markerfacecolor=MODE_COLORS[m],
                          markeredgecolor=SURFACE, markeredgewidth=2)
               for m in MODES]
    fig.legend(handles, [MODE_LABELS[m] for m in MODES],
               loc='lower center', ncol=3, frameon=True, bbox_to_anchor=(0.5, -0.02))
    fig.suptitle('Performance Benchmark Summary', fontsize=14, fontweight='bold', y=1.01)
    fig.tight_layout()
    save_figure(fig, '06_perf_dashboard.png', output_dir)


# ── Scenario B: Stability Charts ───────────────────────────────────

def _stab_xy(data_rows, x_key, y_key, convert=None):
    """Extract x, y lists from stability data rows."""
    xs, ys = [], []
    for r in sorted(data_rows, key=lambda r: int(float(r.get(x_key, 0)))):
        try:
            val = float(r.get(y_key, 0))
        except (ValueError, TypeError):
            val = 0.0
        if convert:
            val = convert(val)
        xs.append(int(float(r.get(x_key, 0))))
        ys.append(val)
    return xs, ys


def plot_stability_cpu(data, output_dir):
    """Chart 7: CPU utilization time-series overlay."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        x, y = _stab_xy(data[mode], 'timestamp', 'cpu_pct')
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.5,
                label=MODE_LABELS[mode], alpha=0.9)
    style_axes(ax, xlabel='Time (seconds)', ylabel='CPU Usage (%)',
               title='CPU Utilization Over Time (Long-Request Stability Test)')
    add_legend(ax)
    save_figure(fig, '07_stability_cpu.png', output_dir)


def plot_stability_memory(data, output_dir):
    """Chart 8: Memory RSS time-series overlay."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        x, y = _stab_xy(data[mode], 'timestamp', 'rss_kb', lambda v: v / 1024.0)
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.5,
                label=MODE_LABELS[mode], alpha=0.9)
    style_axes(ax, xlabel='Time (seconds)', ylabel='Memory RSS (MB)',
               title='Memory Usage Over Time (Long-Request Stability Test)')
    add_legend(ax)
    save_figure(fig, '08_stability_memory.png', output_dir)


def plot_stability_context_switches(data, output_dir):
    """Chart 9: Context switches time-series overlay."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        x, y = _stab_xy(data[mode], 'timestamp', 'cswch_per_sec')
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.5,
                label=MODE_LABELS[mode], alpha=0.9)
    style_axes(ax, xlabel='Time (seconds)', ylabel='Context Switches / sec',
               title='Context Switch Rate Over Time')
    add_legend(ax)
    save_figure(fig, '09_stability_cswch.png', output_dir)


def plot_stability_proc_count(data, output_dir):
    """Chart 10: Process/Thread count time-series overlay."""
    fig, ax = plt.subplots(figsize=FIGSIZE_WIDE)
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        if mode == 'fork':
            x, y = _stab_xy(data[mode], 'timestamp', 'process_count')
            label = f'{MODE_LABELS[mode]} (processes)'
        elif mode == 'pool':
            x, y = _stab_xy(data[mode], 'timestamp', 'thread_count')
            label = f'{MODE_LABELS[mode]} (threads)'
        else:
            x = [int(float(r.get('timestamp', 0))) for r in data[mode]]
            y = [1] * len(x)
            label = f'{MODE_LABELS[mode]} (single process)'
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.5,
                label=label, alpha=0.9)
    style_axes(ax, xlabel='Time (seconds)',
               ylabel='Process / Thread Count',
               title='Concurrency Units Over Time')
    add_legend(ax)
    save_figure(fig, '10_stability_proc_count.png', output_dir)


def plot_stability_dashboard(data, output_dir):
    """Chart 11: 2x2 stability summary dashboard."""
    if not data or len(data) < 2:
        return
    fig, axes = plt.subplots(2, 2, figsize=FIGSIZE_DASHBOARD)

    # CPU
    ax = axes[0, 0]
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        x, y = _stab_xy(data[mode], 'timestamp', 'cpu_pct')
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.2, alpha=0.9)
    style_axes(ax, xlabel='Time (s)', ylabel='CPU (%)')
    ax.set_title('CPU Utilization', fontsize=11, fontweight='semibold')

    # Memory
    ax = axes[0, 1]
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        x, y = _stab_xy(data[mode], 'timestamp', 'rss_kb', lambda v: v / 1024.0)
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.2, alpha=0.9)
    style_axes(ax, xlabel='Time (s)', ylabel='RSS (MB)')
    ax.set_title('Memory Usage', fontsize=11, fontweight='semibold')

    # Context switches
    ax = axes[1, 0]
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        x, y = _stab_xy(data[mode], 'timestamp', 'cswch_per_sec')
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.2, alpha=0.9)
    style_axes(ax, xlabel='Time (s)', ylabel='CS / sec')
    ax.set_title('Context Switches', fontsize=11, fontweight='semibold')

    # Process/Thread count
    ax = axes[1, 1]
    for mode in MODES:
        if mode not in data or not data[mode]:
            continue
        rows = data[mode]
        if mode == 'fork':
            x, y = _stab_xy(rows, 'timestamp', 'process_count')
        elif mode == 'pool':
            x, y = _stab_xy(rows, 'timestamp', 'thread_count')
        else:
            x = [int(float(r.get('timestamp', 0))) for r in rows]
            y = [1] * len(x)
        ax.plot(x, y, color=MODE_COLORS[mode], linewidth=1.2,
                alpha=0.9, label=MODE_LABELS[mode])
    style_axes(ax, xlabel='Time (s)', ylabel='Count')
    ax.set_title('Process / Thread Count', fontsize=11, fontweight='semibold')

    fig.suptitle('Long-Request Stability Test Summary', fontsize=14,
                 fontweight='bold', y=1.01)
    handles = [plt.Line2D([0], [0], color=MODE_COLORS[m], linewidth=2)
               for m in MODES if m in data and data[m]]
    labels = [MODE_LABELS[m] for m in MODES if m in data and data[m]]
    fig.legend(handles, labels, loc='lower center', ncol=3, frameon=True,
               bbox_to_anchor=(0.5, -0.02))
    fig.tight_layout()
    save_figure(fig, '11_stability_dashboard.png', output_dir)


# ── Radar Chart ────────────────────────────────────────────────────

def plot_radar_chart(data, output_dir):
    """Chart 12: Radar/Spider chart comparing all three modes."""
    if not data:
        return

    # Choose 1000-concurrency level (or highest available)
    levels = get_sorted_levels(data)
    ref_level = 1000 if 1000 in levels else levels[-1]

    # Get rows for ref_level
    subset = [r for r in data if r['concurrency'] == ref_level]
    if not subset:
        return

    metrics_config = [
        ('Throughput (QPS)',  'qps',               'max'),
        ('Avg Latency',       'avg_latency_ms',    'min'),
        ('P95 Latency',       'p95_ms',            'min'),
        ('CPU Efficiency',    'cpu_avg_pct',       'min'),
        ('Memory Efficiency', 'rss_avg_kb',        'min'),
        ('Stability (CS/s)',  'cswch_avg_per_sec', 'min'),
    ]

    labels = [m[0] for m in metrics_config]
    n = len(labels)
    angles = np.linspace(0, 2 * np.pi, n, endpoint=False).tolist()
    angles += angles[:1]

    fig, ax = plt.subplots(figsize=FIGSIZE_SQUARE, subplot_kw=dict(polar=True))

    for mode in MODES:
        row = get_mode_row(subset, mode, ref_level)
        if row is None:
            continue
        # Get raw values
        raw = []
        for _, col, _ in metrics_config:
            raw.append(float(row.get(col, 0) or 0))

        # Normalize across modes
        norm = []
        for i, (label, col, direction) in enumerate(metrics_config):
            all_vals = [float(r.get(col, 0) or 0) for r in subset if r.get(col) is not None]
            if not all_vals or max(all_vals) == min(all_vals):
                norm.append(50)
            else:
                mn, mx = min(all_vals), max(all_vals)
                if direction == 'max':
                    norm.append((raw[i] - mn) / (mx - mn) * 100)
                else:
                    norm.append((mx - raw[i]) / (mx - mn) * 100)
        norm += norm[:1]

        ax.fill(angles, norm, alpha=0.08, color=MODE_COLORS[mode])
        ax.plot(angles, norm, color=MODE_COLORS[mode], linewidth=2,
                label=MODE_LABELS[mode], marker='o', markersize=6,
                markerfacecolor=MODE_COLORS[mode],
                markeredgecolor=SURFACE, markeredgewidth=2)

    ax.set_xticks(angles[:-1])
    ax.set_xticklabels(labels, fontsize=9, color=INK_SECONDARY)
    ax.set_ylim(0, 110)
    ax.set_yticks([25, 50, 75, 100])
    ax.set_yticklabels(['25', '50', '75', '100'], fontsize=7, color=INK_MUTED)
    ax.set_title(f'Comprehensive Comparison at {ref_level} Connections',
                 fontsize=13, fontweight='semibold', pad=25)
    add_legend(ax, loc='upper right')
    ax.xaxis.grid(color=GRIDLINE, linewidth=0.5)
    ax.yaxis.grid(color=GRIDLINE, linewidth=0.5)
    ax.spines['polar'].set_color(BASELINE)
    save_figure(fig, '12_radar_comparison.png', output_dir)


# ── Main ────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Generate benchmark comparison charts for mini_web_server')
    parser.add_argument('--perf', help='Path to latency.csv from performance tests')
    parser.add_argument('--stability', help='Path to stability.csv from long-conn tests')
    parser.add_argument('--output', '-o', default='results/charts',
                        help='Output directory for charts (default: results/charts)')
    parser.add_argument('--all', action='store_true',
                        help='Generate all available charts')
    args = parser.parse_args()

    if not args.perf and not args.stability:
        # Try default locations
        script_dir = Path(__file__).parent
        default_perf = script_dir / 'results' / 'perf' / 'latency.csv'
        default_stab = script_dir / 'results' / 'long_conn' / 'stability.csv'
        if default_perf.exists():
            args.perf = str(default_perf)
        if default_stab.exists():
            args.stability = str(default_stab)
        if not args.perf and not args.stability:
            print("ERROR: No data found. Specify --perf and/or --stability.")
            print("Expected locations:")
            print(f"  {default_perf}")
            print(f"  {default_stab}")
            sys.exit(1)

    setup_style()
    os.makedirs(args.output, exist_ok=True)

    perf_data = None
    stability_data = None

    # ── Performance charts ──
    if args.perf:
        perf_data = load_perf_data(args.perf)
        if perf_data is not None:
            print("Generating performance charts...")
            plot_concurrency_vs_latency(perf_data, args.output)
            plot_concurrency_vs_p95(perf_data, args.output)
            plot_concurrency_vs_qps(perf_data, args.output)
            plot_concurrency_vs_cpu(perf_data, args.output)
            plot_concurrency_vs_memory(perf_data, args.output)
            plot_perf_dashboard(perf_data, args.output)

    # ── Stability charts ──
    if args.stability:
        stability_data = load_stability_data(args.stability)
        if stability_data is not None:
            print("Generating stability charts...")
            plot_stability_cpu(stability_data, args.output)
            plot_stability_memory(stability_data, args.output)
            plot_stability_context_switches(stability_data, args.output)
            plot_stability_proc_count(stability_data, args.output)
            plot_stability_dashboard(stability_data, args.output)

    # ── Radar chart (needs perf data) ──
    if perf_data is not None:
        print("Generating radar comparison...")
        plot_radar_chart(perf_data, args.output)

    print(f"\nAll charts saved to: {args.output}/")
    print(f"Total: {len(os.listdir(args.output))} chart(s) generated.")


if __name__ == '__main__':
    main()
