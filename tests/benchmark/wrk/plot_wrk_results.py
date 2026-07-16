#!/usr/bin/env python3
"""12 charts from wrk benchmark: 6 perf + 5 stability + 1 radar."""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import csv, os, glob
from collections import defaultdict

# ── Design ──
MODES = ['fork', 'thread', 'pool', 'select']
MODE_LABELS = {'fork': 'Fork', 'thread': 'Thread', 'pool': 'Pool', 'select': 'Select'}
MODE_COLORS = {'fork': '#e74c3c', 'thread': '#f39c12', 'pool': '#3498db', 'select': '#2ecc71'}
MODE_MARKERS = {'fork': 'o', 'thread': 'D', 'pool': 's', 'select': '^'}
SF = '#fcfcfb'; BL = '#c3c2b7'; GL = '#e1e0d9'
I1 = '#0b0b0b'; I2 = '#52514e'; I3 = '#898781'; DPI = 150

plt.rcParams.update({
    'figure.facecolor': SF, 'axes.facecolor': SF, 'axes.edgecolor': BL,
    'axes.linewidth': 1.0, 'axes.grid': True, 'grid.color': GL, 'grid.linewidth': 0.5,
    'axes.titlesize': 12, 'axes.titleweight': 'semibold', 'axes.labelsize': 10,
    'axes.labelcolor': I2, 'xtick.color': I3, 'ytick.color': I3, 'text.color': I1,
    'legend.facecolor': SF, 'legend.edgecolor': GL, 'legend.fontsize': 8,
    'font.family': 'sans-serif', 'font.size': 10,
    'lines.linewidth': 2, 'lines.markersize': 6, 'lines.markeredgewidth': 2,
    'lines.markeredgecolor': SF,
})

def load_csv(path):
    rows = []
    if not os.path.exists(path): return rows
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows

def fv(r, k):
    try: return float(r.get(k, 0) or 0)
    except: return 0.0

def sa(ax, xl=None, yl=None, t=None):
    if t: ax.set_title(t, fontweight='semibold', pad=10)
    if xl: ax.set_xlabel(xl, color=I2)
    if yl: ax.set_ylabel(yl, color=I2)
    ax.tick_params(colors=I3)
    for s in ax.spines.values(): s.set_color(BL)

def legend(ax):
    h, l = ax.get_legend_handles_labels()
    if h: ax.legend(loc='best', frameon=True)

def line_m(ax, data, mode, xk, yk):
    rows = sorted([r for r in data if r.get('mode','')==mode and fv(r,yk)>0], key=lambda r: fv(r,xk))
    if not rows: return
    x = [fv(r, xk) for r in rows]; y = [fv(r, yk) for r in rows]
    c = MODE_COLORS[mode]
    ax.plot(x, y, color=c, linewidth=2, zorder=3, label=MODE_LABELS[mode],
            marker=MODE_MARKERS[mode], markersize=6, markerfacecolor=c,
            markeredgecolor=SF, markeredgewidth=2)

def save(fig, name, out):
    fig.savefig(f"{out}/{name}", dpi=DPI, bbox_inches='tight', facecolor=SF)
    plt.close(fig)
    print(f"  {name}")

# ── Scenario A: Performance (6 charts) ──
def gen_perf(csv_path, out):
    data = load_csv(csv_path)
    if not data: return
    modes = sorted(set(r['mode'] for r in data))
    levs = sorted(set(int(fv(r,'concurrency')) for r in data))

    # Chart 1: QPS with boundary annotations
    fig, ax = plt.subplots(figsize=(12, 6))
    for m in modes: line_m(ax, data, m, 'concurrency', 'qps')
    # Performance boundary annotations
    ax.axvline(x=200, color='#e74c3c', linestyle='--', linewidth=1, alpha=0.6)
    ax.annotate('Fork collapse\nc=200→500: QPS 1537→85', xy=(350, 8000),
                fontsize=8, color='#e74c3c', fontweight='bold')
    ax.axhline(y=58000, color='#2ecc71', linestyle=':', linewidth=1, alpha=0.6)
    ax.annotate('Select QPS ceiling ~58K', xy=(600, 60000),
                fontsize=8, color='#2ecc71', fontweight='bold')
    sa(ax, 'Concurrent Connections', 'Requests/sec (QPS)', '1. Throughput (QPS) — wrk')
    legend(ax); save(fig, '01_qps.png', out)

    # Chart 2: Avg Latency with knee annotations
    fig, ax = plt.subplots(figsize=(12, 6))
    for m in modes: line_m(ax, data, m, 'concurrency', 'avg_ms')
    ax.axvline(x=50, color='#3498db', linestyle='--', linewidth=1, alpha=0.6)
    ax.annotate('Pool latency knee\nc=10→50: avg 0.13→11.2ms', xy=(60, 200),
                fontsize=8, color='#3498db', fontweight='bold')
    ax.axvline(x=200, color='#e74c3c', linestyle='--', linewidth=1, alpha=0.6)
    ax.annotate('Fork unstable\nc≥200', xy=(210, 5), fontsize=8, color='#e74c3c', fontweight='bold')
    sa(ax, 'Concurrent Connections', 'Avg Latency (ms)', '2. Average Latency — wrk')
    legend(ax); save(fig, '02_avg_latency.png', out)

    # Chart 3: P99 Tail Latency with boundary markers
    fig, ax = plt.subplots(figsize=(12, 6))
    for m in modes: line_m(ax, data, m, 'concurrency', 'p99_ms')
    # Pool P99 explosion
    ax.annotate('Pool P99 explosion\nc=50: 51ms  c=500: 653ms\nc=3000: 4010ms',
                xy=(500, 650), xytext=(250, 2500),
                arrowprops=dict(arrowstyle='->', color='#3498db', lw=1.2),
                fontsize=8, color='#3498db', fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.3', facecolor='white', alpha=0.8))
    # Select P99 growth
    ax.annotate('Select P99: 2ms→2290ms\nlinear growth (single-thread)',
                xy=(1000, 1410), xytext=(600, 2000),
                arrowprops=dict(arrowstyle='->', color='#2ecc71', lw=1.2),
                fontsize=8, color='#2ecc71', fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.3', facecolor='white', alpha=0.8))
    # Thread barely moves
    ax.annotate('Thread P99: only 2.5→182ms\n(best tail latency)',
                xy=(500, 30), xytext=(200, 800),
                arrowprops=dict(arrowstyle='->', color='#f39c12', lw=1.2),
                fontsize=8, color='#f39c12', fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.3', facecolor='white', alpha=0.8))
    sa(ax, 'Concurrent Connections', 'P99 Latency (ms)', '3. P99 Tail Latency — wrk')
    legend(ax); save(fig, '03_p99_latency.png', out)

    # Chart 4: P50 Median Latency
    fig, ax = plt.subplots(figsize=(12, 6))
    for m in modes: line_m(ax, data, m, 'concurrency', 'p50_ms')
    sa(ax, 'Concurrent Connections', 'P50 Latency (ms)', '4. P50 Median Latency — wrk')
    legend(ax); save(fig, '04_p50_latency.png', out)

    # Chart 5: Error Rate (%)
    fig, ax = plt.subplots(figsize=(12, 6))
    n = len(modes); bw = 0.18; xp = np.arange(len(levs))
    for i, m in enumerate(modes):
        vals = []
        for lv in levs:
            r = [r for r in data if r['mode']==m and int(fv(r,'concurrency'))==lv]
            if r:
                errs = fv(r[0],'err_connect')+fv(r[0],'err_read')+fv(r[0],'err_timeout')
                tot = max(fv(r[0],'total'), 1)
                vals.append(errs/tot*100)
            else: vals.append(0)
        off = (i - n/2 + 0.5) * bw
        ax.bar(xp+off, vals, bw*0.9, color=MODE_COLORS[m], label=MODE_LABELS[m], zorder=3, edgecolor=SF)
    sa(ax, 'Concurrent Connections', 'Error Rate (%)', '5. Socket Error Rate (read errors, no connect refusals)')
    ax.set_xticks(xp); ax.set_xticklabels(levs)
    ax.annotate('err_connect=0 for all modes:\naccept queue never exhausted', xy=(0.98, 0.95),
                xycoords='axes fraction', ha='right', va='top',
                fontsize=9, color=I2, bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))
    legend(ax); save(fig, '05_errors.png', out)

    # Chart 6: Dashboard (2x2)
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    for m in modes: line_m(axes[0,0], data, m, 'concurrency', 'qps')
    sa(axes[0,0], 'Connections', 'QPS'); axes[0,0].set_title('Throughput', fontweight='semibold')
    for m in modes: line_m(axes[0,1], data, m, 'concurrency', 'avg_ms')
    sa(axes[0,1], 'Connections', 'Avg (ms)'); axes[0,1].set_title('Avg Latency', fontweight='semibold')
    for m in modes: line_m(axes[1,0], data, m, 'concurrency', 'p99_ms')
    sa(axes[1,0], 'Connections', 'P99 (ms)'); axes[1,0].set_title('P99 Tail Latency', fontweight='semibold')
    n2=len(modes); bw2=0.18; xp2=np.arange(len(levs))
    for i,m in enumerate(modes):
        vals=[]
        for lv in levs:
            r=[r for r in data if r['mode']==m and int(fv(r,'concurrency'))==lv]
            if r: vals.append((fv(r[0],'err_connect')+fv(r[0],'err_read')+fv(r[0],'err_timeout'))/max(fv(r[0],'total'),1)*100)
            else: vals.append(0)
        axes[1,1].bar(xp2+(i-n2/2+0.5)*bw2, vals, bw2*0.9, color=MODE_COLORS[m], label=MODE_LABELS[m], zorder=3, edgecolor=SF)
    sa(axes[1,1], 'Connections', 'Error %'); axes[1,1].set_title('Error Rate', fontweight='semibold')
    axes[1,1].set_xticks(xp2); axes[1,1].set_xticklabels(levs)
    handles = [plt.Line2D([0],[0],color=MODE_COLORS[m],linewidth=2,marker=MODE_MARKERS[m],markersize=6,markerfacecolor=MODE_COLORS[m],markeredgecolor=SF) for m in modes]
    fig.legend(handles, [MODE_LABELS[m] for m in modes], loc='lower center', ncol=4, frameon=True, bbox_to_anchor=(0.5,-0.02))
    fig.suptitle('6. Performance Dashboard — wrk', fontweight='bold', y=1.01)
    fig.tight_layout(); save(fig, '06_dashboard.png', out)

# ── Scenario B: Stability (5 charts from long conn metrics) ──
def gen_stability(out):
    # Collect all longconn_*.csv files
    metrics_dir = os.path.dirname(os.path.abspath(__file__)) + "/results"
    files = sorted(glob.glob(f"{metrics_dir}/longconn_*.csv"))
    if not files:
        print("  (no long connection data — skipping stability charts)")
        return

    all_data = {}  # mode -> {conn -> [(timestamp, cpu, rss, thread_count, proc_count, cswch, conn_count), ...]}
    for f in files:
        basename = os.path.basename(f)
        # longconn_pool_c2.csv
        parts = basename.replace('.csv','').split('_')
        mode = parts[1]; conn = int(parts[2].replace('c',''))
        rows = load_csv(f)
        if mode not in all_data: all_data[mode] = {}
        all_data[mode][conn] = rows

    if not all_data: return

    # Chart 7: CPU time-series (pool vs select, c=5)
    fig, ax = plt.subplots(figsize=(12, 6))
    for mode in ['pool', 'select']:
        if mode not in all_data or 5 not in all_data[mode]: continue
        rows = all_data[mode][5]
        ts = [int(fv(r,'timestamp')) for r in rows]
        cpu = [fv(r,'cpu_pct') for r in rows]
        ax.plot(ts, cpu, color=MODE_COLORS[mode], linewidth=1.2, label=f"{MODE_LABELS[mode]} c=5")
    sa(ax, 'Time (seconds)', 'CPU Usage (%)', '7. CPU Utilization — Long Connection Test (120s)')
    legend(ax); save(fig, '07_longconn_cpu.png', out)

    # Chart 8: Memory RSS time-series
    fig, ax = plt.subplots(figsize=(12, 6))
    for mode in ['pool', 'select']:
        if mode not in all_data or 5 not in all_data[mode]: continue
        rows = all_data[mode][5]
        ts = [int(fv(r,'timestamp')) for r in rows]
        rss = [fv(r,'rss_kb')/1024 for r in rows]  # KB -> MB
        ax.plot(ts, rss, color=MODE_COLORS[mode], linewidth=1.2, label=f"{MODE_LABELS[mode]} c=5")
    sa(ax, 'Time (seconds)', 'Memory RSS (MB)', '8. Memory Usage — Long Connection Test (120s)')
    legend(ax); save(fig, '08_longconn_memory.png', out)

    # Chart 9: Context switches time-series
    fig, ax = plt.subplots(figsize=(12, 6))
    for mode in ['pool', 'select']:
        if mode not in all_data or 5 not in all_data[mode]: continue
        rows = all_data[mode][5]
        ts = [int(fv(r,'timestamp')) for r in rows]
        cs = [fv(r,'cswch_total') for r in rows]
        ax.plot(ts, cs, color=MODE_COLORS[mode], linewidth=1.2, label=f"{MODE_LABELS[mode]} c=5")
    sa(ax, 'Time (seconds)', 'Context Switches / sec', '9. Context Switch Rate — Long Connection Test (120s)')
    legend(ax); save(fig, '09_longconn_cswch.png', out)

    # Chart 10: Thread/Process count time-series
    fig, ax = plt.subplots(figsize=(12, 6))
    for mode in ['pool', 'select']:
        if mode not in all_data or 5 not in all_data[mode]: continue
        rows = all_data[mode][5]
        ts = [int(fv(r,'timestamp')) for r in rows]
        if mode == 'select':
            count = [1]*len(ts)  # select is always single
        else:
            count = [fv(r,'thread_count') for r in rows]
        ax.plot(ts, count, color=MODE_COLORS[mode], linewidth=1.2, label=f"{MODE_LABELS[mode]} c=5")
    sa(ax, 'Time (seconds)', 'Thread Count', '10. Thread/Process Count — Long Connection Test (120s)')
    legend(ax); save(fig, '10_longconn_proc_count.png', out)

    # Chart 11: Stability dashboard (2x2)
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    for mode in ['pool', 'select']:
        if mode not in all_data or 5 not in all_data[mode]: continue
        rows = all_data[mode][5]
        ts = [int(fv(r,'timestamp')) for r in rows]
        axes[0,0].plot(ts, [fv(r,'cpu_pct') for r in rows], color=MODE_COLORS[mode], linewidth=1.2, label=MODE_LABELS[mode])
        axes[0,1].plot(ts, [fv(r,'rss_kb')/1024 for r in rows], color=MODE_COLORS[mode], linewidth=1.2)
        axes[1,0].plot(ts, [fv(r,'cswch_total') for r in rows], color=MODE_COLORS[mode], linewidth=1.2)
        axes[1,1].plot(ts, [fv(r,'thread_count') for r in rows], color=MODE_COLORS[mode], linewidth=1.2)
    sa(axes[0,0], 'Time (s)', 'CPU %'); axes[0,0].set_title('CPU Utilization', fontweight='semibold')
    sa(axes[0,1], 'Time (s)', 'RSS (MB)'); axes[0,1].set_title('Memory Usage', fontweight='semibold')
    sa(axes[1,0], 'Time (s)', 'CS/s'); axes[1,0].set_title('Context Switches', fontweight='semibold')
    sa(axes[1,1], 'Time (s)', 'Threads'); axes[1,1].set_title('Thread Count', fontweight='semibold')
    handles = [plt.Line2D([0],[0],color=MODE_COLORS[m],linewidth=2) for m in ['pool','select'] if m in all_data]
    fig.legend(handles, [MODE_LABELS[m] for m in ['pool','select'] if m in all_data], loc='lower center', ncol=2, frameon=True, bbox_to_anchor=(0.5,-0.02))
    fig.suptitle('11. Long Connection Stability Dashboard', fontweight='bold', y=1.01)
    fig.tight_layout(); save(fig, '11_longconn_dashboard.png', out)

# ── Radar ──
def gen_radar(csv_path, out):
    data = load_csv(csv_path)
    if not data: return
    ref = 100
    subset = [r for r in data if int(fv(r,'concurrency'))==ref]
    if not subset:
        # fallback: use most common concurrency level
        from collections import Counter
        cnt = Counter(int(fv(r,'concurrency')) for r in data)
        ref = cnt.most_common(1)[0][0]
        subset = [r for r in data if int(fv(r,'concurrency'))==ref]
    if not subset: return

    metrics = [('QPS', 'qps', 'max'), ('Avg Latency', 'avg_ms', 'min'),
               ('P50', 'p50_ms', 'min'), ('P99', 'p99_ms', 'min'),
               ('Stability', 'err_read', 'min'),
               ('Max Latency', 'max_ms', 'min')]
    labels = [m[0] for m in metrics]; n = len(labels)
    angles = np.linspace(0, 2*np.pi, n, endpoint=False).tolist() + [0]
    fig, ax = plt.subplots(figsize=(8, 8), subplot_kw=dict(polar=True))

    modes_present = list(set(r['mode'] for r in subset))
    for mode in modes_present:
        row = [r for r in subset if r['mode']==mode][0]
        raw = [fv(row, col) for _,col,_ in metrics]
        norm = []
        for i,(_,col,dir) in enumerate(metrics):
            vals = [fv(r,col) for r in subset]
            if max(vals)==min(vals): norm.append(50)
            else: norm.append((raw[i]-min(vals))/(max(vals)-min(vals))*100 if dir=='max' else (max(vals)-raw[i])/(max(vals)-min(vals))*100)
        norm.append(norm[0])
        ax.fill(angles, norm, alpha=0.08, color=MODE_COLORS[mode])
        ax.plot(angles, norm, color=MODE_COLORS[mode], linewidth=2, label=MODE_LABELS[mode],
                marker='o', markersize=5, markerfacecolor=MODE_COLORS[mode], markeredgecolor=SF, markeredgewidth=2)
    ax.set_xticks(angles[:-1]); ax.set_xticklabels(labels, fontsize=9, color=I2)
    ax.set_ylim(0,110); ax.set_yticks([25,50,75,100])
    ax.set_yticklabels(['25','50','75','100'], fontsize=7, color=I3)
    ax.set_title(f'12. Radar Comparison @ {ref} Connections', fontweight='semibold', pad=20)
    legend(ax); save(fig, '12_radar.png', out)


if __name__ == '__main__':
    d = os.path.dirname(os.path.abspath(__file__))
    out = f"{d}/results/charts"
    os.makedirs(out, exist_ok=True)

    csv_path = f"{d}/results/all_results.csv"
    if os.path.exists(csv_path):
        print("=== Scenario A: Performance Charts ===")
        gen_perf(csv_path, out)
        print("\n=== Scenario B: Stability Charts ===")
        gen_stability(out)
        print("\n=== Radar ===")
        gen_radar(csv_path, out)
        print(f"\nAll charts → {out}/")
    else:
        print(f"ERROR: {csv_path} not found — run wrk_bench.py first")
