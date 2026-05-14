#!/usr/bin/env bash
# Run every module's benchmark and dump a clean BENCHMARKS.md file at repo root.
#
# Usage:
#   scripts/run_benchmarks.sh             # writes BENCHMARKS.md
#   scripts/run_benchmarks.sh /tmp/x.md   # writes to a custom path
#
# The output is a markdown report with one section per module. Each section
# is the last ~10 lines of the binary's output (results, not boilerplate).
set -euo pipefail

OUT="${1:-BENCHMARKS.md}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

# Build everything first; if any binary is missing the report would be sparse.
make build >/dev/null

run_bench() {
    local title="$1"; shift
    echo "## ${title}"
    echo
    echo '```'
    # shellcheck disable=SC2068
    $@ 2>&1 | tail -n 12
    echo '```'
    echo
}

{
    echo "# Measured Benchmarks"
    echo
    echo "_Last run: $(date -u +'%Y-%m-%dT%H:%M:%SZ')_"
    echo "_Host: $(uname -srm)_"
    echo "_Compiler: $(${CXX:-g++} --version | head -1)_"
    echo
    echo "Regenerate with \`scripts/run_benchmarks.sh\`."
    echo

    run_bench "Orderbook (basic)"                ./orderbook/orderbook
    run_bench "Orderbook benchmark"              ./orderbook/benchmark_orderbook
    run_bench "Orderbook latency histogram"      ./orderbook/latency_histogram 100000
    run_bench "Lock-free SPSC queue"             ./lockfree/spsc_queue
    run_bench "Memory / cache latency"           ./memory-latency/cache_latency
    run_bench "ITCH parser"                      ./itch-parser/benchmark_itch
    run_bench "Latency benchmark"                ./benchmarks/latency_benchmark 50000
    run_bench "Orderbook E2E benchmark"          ./benchmarks/orderbook_benchmark 50000
    run_bench "OMS"                              ./oms/oms_demo 100000
    run_bench "Risk Manager"                     ./risk/risk_demo 100000
    run_bench "Smart Router"                     ./router/router_demo 100000
    run_bench "Trade Logger"                     ./logger/logger_demo 100000
    run_bench "Mean Reversion Strategy"          ./strategy/strategy_demo 100000
    run_bench "FIX parser"                       ./fix-protocol/fix_demo 100000
    run_bench "OUCH protocol"                    ./ouch-protocol/ouch_demo 100000
    run_bench "Market Simulator (full)"          ./simulator/sim_demo 50000
} > "$OUT"

echo "Wrote $OUT"
