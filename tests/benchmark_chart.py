#!/usr/bin/env python3
"""
HFT Benchmark Chart Generator
Generator wykresów benchmarków HFT

Generates PNG charts from benchmark results for README display.
Generuje wykresy PNG z wyników benchmarków do wyświetlania w README.
Requires: matplotlib (pip install matplotlib)
"""
import subprocess
import re
import sys
import os

def run_orderbook_benchmark() -> dict:
    """Run C++ orderbook benchmark and parse results.
    Uruchom benchmark C++ orderbooka i sparsuj wyniki.
    """
    binary = os.path.join(os.path.dirname(__file__), '..', 'orderbook', 'benchmark_orderbook')
    if not os.path.exists(binary):
        print("ERROR: benchmark_orderbook not found. Run 'make build' first.")
        sys.exit(1)

    output = subprocess.check_output([binary], text=True)
    results = {'sizes': [], 'ops_per_sec': [], 'time_ms': []}

    for line in output.strip().split('\n'):
        parts = line.split()
        if parts and parts[0].isdigit():
            results['sizes'].append(int(parts[0]))
            results['time_ms'].append(float(parts[1]))
            results['ops_per_sec'].append(int(parts[2]))
    return results


def run_latency_histogram() -> dict:
    """Run C++ latency histogram and parse percentiles.
    Uruchom histogram opóźnień C++ i sparsuj percentyle.
    """
    binary = os.path.join(os.path.dirname(__file__), '..', 'orderbook', 'latency_histogram')
    if not os.path.exists(binary):
        print("ERROR: latency_histogram not found. Run 'make build' first.")
        sys.exit(1)

    output = subprocess.check_output([binary, '1000000'], text=True)
    results = {}

    for line in output.strip().split('\n'):
        line = line.strip()
        if line.startswith('p50'):
            results['p50'] = float(line.split(':')[1])
        elif line.startswith('p95'):
            results['p95'] = float(line.split(':')[1])
        elif line.startswith('p99 '):
            results['p99'] = float(line.split(':')[1])
        elif line.startswith('p99.9'):
            results['p999'] = float(line.split(':')[1])
        elif line.startswith('max'):
            results['max'] = float(line.split(':')[1])
        elif line.startswith('Throughput'):
            results['throughput'] = int(line.split(':')[1].strip().split()[0])
    return results


def generate_charts(ob_results: dict, lat_results: dict, output_dir: str) -> None:
    """Generate benchmark PNG charts.
    Generuj wykresy PNG z benchmarków.
    """
    try:
        import matplotlib
        matplotlib.use('Agg')  # headless backend
        import matplotlib.pyplot as plt
        import matplotlib.ticker as ticker
    except ImportError:
        print("ERROR: matplotlib not installed. Run: pip install matplotlib")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    # --- Chart 1: Orderbook Throughput ---
    fig, ax = plt.subplots(figsize=(10, 5))
    colors = ['#2196F3', '#4CAF50', '#FF9800', '#F44336']
    bars = ax.bar(
        [f'{s//1000}K' if s < 1_000_000 else f'{s//1_000_000}M' for s in ob_results['sizes']],
        [ops / 1_000_000 for ops in ob_results['ops_per_sec']],
        color=colors, edgecolor='white', linewidth=1.5
    )

    for bar, ops in zip(bars, ob_results['ops_per_sec']):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.2,
                f'{ops/1_000_000:.1f}M', ha='center', va='bottom',
                fontweight='bold', fontsize=11)

    ax.set_title('Order Book Throughput (Fixed-Point int64 Prices)', fontsize=14, fontweight='bold')
    ax.set_xlabel('Number of Orders', fontsize=12)
    ax.set_ylabel('Million Orders / Second', fontsize=12)
    ax.set_ylim(0, max(ops / 1_000_000 for ops in ob_results['ops_per_sec']) * 1.25)
    ax.grid(axis='y', alpha=0.3)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    plt.tight_layout()
    path1 = os.path.join(output_dir, 'throughput.png')
    plt.savefig(path1, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {path1}")

    # --- Chart 2: Latency Percentiles ---
    fig, ax = plt.subplots(figsize=(10, 5))
    percentiles = ['p50', 'p95', 'p99', 'p99.9', 'max']
    values = [lat_results.get('p50', 0), lat_results.get('p95', 0),
              lat_results.get('p99', 0), lat_results.get('p999', 0),
              lat_results.get('max', 0)]
    colors_lat = ['#4CAF50', '#8BC34A', '#FF9800', '#FF5722', '#F44336']

    bars = ax.bar(percentiles, values, color=colors_lat, edgecolor='white', linewidth=1.5)

    for bar, val in zip(bars, values):
        label = f'{val:.0f}ns'
        if val >= 1000:
            label = f'{val/1000:.1f}\u00b5s'
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(values) * 0.02,
                label, ha='center', va='bottom', fontweight='bold', fontsize=11)

    ax.set_title(f'Per-Order Latency Distribution (1M orders, {lat_results.get("throughput", 0)/1_000_000:.1f}M ops/sec)',
                 fontsize=14, fontweight='bold')
    ax.set_xlabel('Percentile', fontsize=12)
    ax.set_ylabel('Latency (ns)', fontsize=12)
    ax.set_yscale('log')
    ax.grid(axis='y', alpha=0.3)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    plt.tight_layout()
    path2 = os.path.join(output_dir, 'latency.png')
    plt.savefig(path2, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {path2}")


def main() -> None:
    print("=== HFT Benchmark Chart Generator ===\n")

    print("[1/3] Running orderbook throughput benchmark...")
    ob = run_orderbook_benchmark()
    print(f"  Results: {len(ob['sizes'])} data points\n")

    print("[2/3] Running latency histogram (1M orders)...")
    lat = run_latency_histogram()
    print(f"  Throughput: {lat.get('throughput', 0):,} ops/sec")
    print(f"  p50={lat.get('p50', 0):.0f}ns  p99={lat.get('p99', 0):.0f}ns  max={lat.get('max', 0):.0f}ns\n")

    output_dir = os.path.join(os.path.dirname(__file__), '..', 'docs')
    print("[3/3] Generating charts...")
    generate_charts(ob, lat, output_dir)

    print(f"\nDone! Charts saved to docs/throughput.png and docs/latency.png")
    print("Add to README with:")
    print("  ![Throughput](docs/throughput.png)")
    print("  ![Latency](docs/latency.png)")


if __name__ == '__main__':
    main()
