#!/usr/bin/env python3
"""
HFT Benchmark Chart Generator
Generator wykresów benchmarków HFT

Generates PNG charts from C++ benchmark results for README display.
Generuje wykresy PNG z wyników benchmarków C++ do wyświetlania w README.
Requires: matplotlib (pip install matplotlib)
"""
import subprocess
import re
import sys
import os


def run_and_capture(binary, args=None):
    """Run a binary and return its stdout."""
    cmd = [binary] + (args or [])
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        print(f"  WARNING: {binary} failed: {e}")
        return ""


def parse_throughput(output, pattern=r'Throughput:\s+([\d.]+)\s+M'):
    """Extract throughput in M ops/sec from output."""
    m = re.search(pattern, output)
    return float(m.group(1)) if m else 0.0


def collect_all_benchmarks(project_root):
    """Run all C++ benchmarks and collect throughput + latency data.
    Uruchom wszystkie benchmarki C++ i zbierz dane throughput + latency.
    """
    results = {}

    # Orderbook — uses largest size (1M orders)
    # Columns: Orders  Time(ms)  Orders/sec  Trades  Depth
    out = run_and_capture(os.path.join(project_root, 'orderbook', 'benchmark_orderbook'))
    for line in out.strip().split('\n'):
        parts = line.split()
        if parts and parts[0] == '1000000':
            results['Order Book'] = {'throughput': int(parts[2]) / 1_000_000}
            break

    # Latency histogram — orderbook latency percentiles
    out = run_and_capture(os.path.join(project_root, 'orderbook', 'latency_histogram'), ['500000'])
    latency = {}
    for line in out.strip().split('\n'):
        line = line.strip()
        if line.startswith('p50'):
            latency['p50'] = float(line.split(':')[1])
        elif line.startswith('p95'):
            latency['p95'] = float(line.split(':')[1])
        elif line.startswith('p99 '):
            latency['p99'] = float(line.split(':')[1])
        elif line.startswith('p99.9'):
            latency['p999'] = float(line.split(':')[1])
        elif line.startswith('max') or line.startswith('Max'):
            latency['max'] = float(line.split(':')[1])
    results['Order Book']['latency'] = latency

    # All demo binaries with standard output format
    demos = [
        ('oms/oms_demo', 'OMS', '100000'),
        ('risk/risk_demo', 'Risk Manager', '100000'),
        ('router/router_demo', 'Smart Router', '100000'),
        ('logger/logger_demo', 'Trade Logger', '100000'),
        ('strategy/strategy_demo', 'Mean Reversion', '100000'),
        ('fix-protocol/fix_demo', 'FIX 4.2 Parser', '100000'),
        ('ouch-protocol/ouch_demo', 'OUCH 4.2 Encoder', '100000'),
    ]

    for binary_path, name, count in demos:
        out = run_and_capture(os.path.join(project_root, binary_path), [count])
        tp = parse_throughput(out)
        lat = {}
        for line in out.strip().split('\n'):
            line = line.strip()
            if 'p50:' in line:
                m = re.search(r'p50:\s+(\d+)', line)
                if m:
                    lat['p50'] = float(m.group(1))
            elif 'p99:' in line and 'p99.9' not in line:
                m = re.search(r'p99:\s+(\d+)', line)
                if m:
                    lat['p99'] = float(m.group(1))
        results[name] = {'throughput': tp, 'latency': lat}

    # ITCH parser — says "million msg/sec"
    out = run_and_capture(os.path.join(project_root, 'itch-parser', 'benchmark_itch'))
    m = re.search(r'([\d.]+)\s+million msg/sec', out)
    if m:
        results['ITCH 5.0 Parser'] = {'throughput': float(m.group(1)), 'latency': {}}
    for line in out.strip().split('\n'):
        line = line.strip()
        if 'p50:' in line and 'ITCH 5.0 Parser' in results:
            m_lat = re.search(r'p50:\s+(\d+)', line)
            if m_lat:
                results['ITCH 5.0 Parser']['latency']['p50'] = float(m_lat.group(1))
                break

    # DPDK — poll mode throughput
    out = run_and_capture(os.path.join(project_root, 'dpdk-bypass', 'dpdk_demo'), ['50000'])
    m = re.search(r'Poll throughput:\s+([\d.]+)\s+M', out)
    if m:
        results['DPDK Poll Mode'] = {'throughput': float(m.group(1)), 'latency': {}}

    # Lock-free SPSC queue — raw number "17667844 msg/sec"
    out = run_and_capture(os.path.join(project_root, 'lockfree', 'spsc_queue'))
    m = re.search(r'Throughput:\s+(\d+)\s+msg/sec', out)
    if m:
        results['SPSC Queue'] = {'throughput': int(m.group(1)) / 1_000_000, 'latency': {}}

    return results


def generate_charts(results, output_dir):
    """Generate two professional benchmark charts.
    Generuj dwa profesjonalne wykresy benchmarków.
    """
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print("ERROR: matplotlib not installed. Run: pip install matplotlib")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    # Sort by throughput descending
    sorted_modules = sorted(results.items(), key=lambda x: x[1]['throughput'], reverse=True)
    names = [m[0] for m in sorted_modules]
    throughputs = [m[1]['throughput'] for m in sorted_modules]

    # --- Chart 1: All-Module Throughput Bar Chart ---
    fig, ax = plt.subplots(figsize=(12, 6))

    # Color gradient: fastest=dark blue, slowest=light blue
    n = len(names)
    colors = [plt.cm.Blues(0.4 + 0.5 * (n - i) / n) for i in range(n)]

    bars = ax.barh(range(n), throughputs, color=colors, edgecolor='white', linewidth=0.8, height=0.7)

    # Labels on bars
    for bar, tp in zip(bars, throughputs):
        ax.text(bar.get_width() + max(throughputs) * 0.01,
                bar.get_y() + bar.get_height() / 2,
                f'{tp:.1f}M/sec', va='center', fontsize=10, fontweight='bold')

    ax.set_yticks(range(n))
    ax.set_yticklabels(names, fontsize=11)
    ax.invert_yaxis()
    ax.set_xlabel('Million Operations / Second', fontsize=12)
    ax.set_title('HFT Module Throughput — C++ (VirtualBox 2-core VM)',
                 fontsize=14, fontweight='bold', pad=15)
    ax.set_xlim(0, max(throughputs) * 1.2)
    ax.grid(axis='x', alpha=0.3)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    plt.tight_layout()
    path1 = os.path.join(output_dir, 'throughput.png')
    plt.savefig(path1, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {path1}")

    # --- Chart 2: Latency Comparison (p50 vs p99) ---
    # Only modules with latency data
    lat_modules = [(name, data) for name, data in sorted_modules
                   if data.get('latency', {}).get('p50')]
    if not lat_modules:
        print("  WARNING: No latency data found, skipping latency chart")
        return

    lat_names = [m[0] for m in lat_modules]
    p50s = [m[1]['latency']['p50'] for m in lat_modules]
    p99s = [m[1]['latency'].get('p99', m[1]['latency']['p50']) for m in lat_modules]

    # Sort by p50 ascending
    combined = sorted(zip(lat_names, p50s, p99s), key=lambda x: x[1])
    lat_names = [c[0] for c in combined]
    p50s = [c[1] for c in combined]
    p99s = [c[2] for c in combined]

    fig, ax = plt.subplots(figsize=(12, 6))
    x = range(len(lat_names))
    width = 0.35

    bars1 = ax.barh([i - width / 2 for i in x], p50s, width,
                    label='p50 (median)', color='#2196F3', edgecolor='white')
    bars2 = ax.barh([i + width / 2 for i in x], p99s, width,
                    label='p99 (tail)', color='#FF9800', edgecolor='white')

    # Labels
    for bar, val in zip(bars1, p50s):
        ax.text(bar.get_width() + max(p99s) * 0.01,
                bar.get_y() + bar.get_height() / 2,
                f'{val:.0f}ns', va='center', fontsize=9)
    for bar, val in zip(bars2, p99s):
        ax.text(bar.get_width() + max(p99s) * 0.02,
                bar.get_y() + bar.get_height() / 2,
                f'{val:.0f}ns', va='center', fontsize=9)

    ax.set_yticks(list(x))
    ax.set_yticklabels(lat_names, fontsize=11)
    ax.invert_yaxis()
    ax.set_xlabel('Latency (nanoseconds)', fontsize=12)
    ax.set_title('HFT Module Latency — p50 vs p99 (VirtualBox 2-core VM)',
                 fontsize=14, fontweight='bold', pad=15)
    ax.set_xlim(0, max(p99s) * 1.35)
    ax.legend(loc='lower right', fontsize=11)
    ax.grid(axis='x', alpha=0.3)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    plt.tight_layout()
    path2 = os.path.join(output_dir, 'latency.png')
    plt.savefig(path2, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {path2}")


def main():
    print("=== HFT Benchmark Chart Generator ===\n")
    project_root = os.path.join(os.path.dirname(__file__), '..')

    print("[1/2] Running all C++ benchmarks...")
    results = collect_all_benchmarks(project_root)
    print(f"  Collected {len(results)} modules:\n")
    for name, data in sorted(results.items(), key=lambda x: -x[1]['throughput']):
        lat_str = ""
        if data.get('latency', {}).get('p50'):
            lat_str = f"  (p50={data['latency']['p50']:.0f}ns)"
        print(f"    {name:20s}  {data['throughput']:6.1f} M/sec{lat_str}")
    print()

    output_dir = os.path.join(project_root, 'docs')
    print("[2/2] Generating charts...")
    generate_charts(results, output_dir)

    print(f"\nDone! Charts saved to docs/")


if __name__ == '__main__':
    main()
