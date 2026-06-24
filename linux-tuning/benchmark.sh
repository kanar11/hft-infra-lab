#!/bin/bash
# HFT Latency Benchmark
# Author: Kasper Kanarek
# Description: Automated cyclictest benchmark before and after tuning
# Automated cyclictest performance test before and after tuning

DURATION=10000
PRIORITY=80
LOG_DIR="benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p $LOG_DIR

echo "=== HFT Latency Benchmark ==="

# Verify the required real-time testing tool is installed before proceeding
# Verify the required real-time testing tool is installed before continuing
if ! command -v cyclictest &> /dev/null; then
    echo "cyclictest not found. Install: sudo dnf install rt-tests"
    exit 1
fi

# Measure baseline latency before applying HFT tuning to establish a performance baseline for comparison
# Measure baseline latency before applying HFT tuning to establish a performance baseline for comparison
echo "[1/2] Running baseline benchmark..."
sudo cyclictest -l $DURATION -t 1 -p $PRIORITY -i 200 -q \
    > $LOG_DIR/baseline_$TIMESTAMP.txt 2>&1
echo "Baseline saved to: $LOG_DIR/baseline_$TIMESTAMP.txt"

# Extract minimum, average, and maximum latency values from baseline test results
# Extract minimum, mean and maximum latency values from the baseline test results
BASELINE_MIN=$(grep "Min:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')
BASELINE_AVG=$(grep "Avg:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')
BASELINE_MAX=$(grep "Max:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')

# Execute all HFT tuning optimizations to reduce system latency
# Apply all HFT tuning optimizations to reduce system latency
echo "[2/2] Applying tuning..."
sudo ./hft_tuning.sh > /dev/null 2>&1

# Run latency benchmark again after tuning to measure improvement achieved by kernel configuration changes
# Run the latency performance test again after tuning to measure the improvement from the kernel configuration changes
echo "[3/3] Running post-tuning benchmark..."
sudo cyclictest -l $DURATION -t 1 -p $PRIORITY -i 200 -q \
    > $LOG_DIR/tuned_$TIMESTAMP.txt 2>&1
echo "Tuned saved to: $LOG_DIR/tuned_$TIMESTAMP.txt"

# Extract latency metrics from post-tuning test for comparison with baseline results
# Extract latency metrics from the post-tuning test to compare with the baseline results
TUNED_MIN=$(grep "Min:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')
TUNED_AVG=$(grep "Avg:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')
TUNED_MAX=$(grep "Max:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')

# Display comparison table showing the latency improvements achieved by HFT tuning
# Print a comparison table showing the latency improvements achieved by HFT tuning
echo ""
echo "=== Results ==="
echo "Metric    | Baseline | Tuned"
echo "----------|----------|------"
echo "Min (µs)  | $BASELINE_MIN      | $TUNED_MIN"
echo "Avg (µs)  | $BASELINE_AVG      | $TUNED_AVG"
echo "Max (µs)  | $BASELINE_MAX      | $TUNED_MAX"
echo ""
echo "Full results in: $LOG_DIR/"
