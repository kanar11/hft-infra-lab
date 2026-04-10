#!/bin/bash
# HFT Latency Benchmark
# Author: Kasper Kanarek
# Description: Automated cyclictest benchmark before and after tuning

DURATION=10000
PRIORITY=80
LOG_DIR="benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p $LOG_DIR

echo "=== HFT Latency Benchmark ==="

# Check cyclictest installed
if ! command -v cyclictest &> /dev/null; then
    echo "cyclictest not found. Install: sudo apt install rt-tests"
    exit 1
fi

# 1. Baseline measurement
echo "[1/2] Running baseline benchmark..."
sudo cyclictest -l $DURATION -t 1 -p $PRIORITY -i 200 -q \
    > $LOG_DIR/baseline_$TIMESTAMP.txt 2>&1
echo "Baseline saved to: $LOG_DIR/baseline_$TIMESTAMP.txt"

# Parse baseline results
BASELINE_MIN=$(grep "Min:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')
BASELINE_AVG=$(grep "Avg:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')
BASELINE_MAX=$(grep "Max:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')

# 2. Apply tuning
echo "[2/2] Applying tuning..."
sudo ./hft_tuning.sh > /dev/null 2>&1

# 3. Post-tuning measurement
echo "[3/3] Running post-tuning benchmark..."
sudo cyclictest -l $DURATION -t 1 -p $PRIORITY -i 200 -q \
    > $LOG_DIR/tuned_$TIMESTAMP.txt 2>&1
echo "Tuned saved to: $LOG_DIR/tuned_$TIMESTAMP.txt"

# Parse tuned results
TUNED_MIN=$(grep "Min:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')
TUNED_AVG=$(grep "Avg:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')
TUNED_MAX=$(grep "Max:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')

# 4. Summary
echo ""
echo "=== Results ==="
echo "Metric    | Baseline | Tuned"
echo "----------|----------|------"
echo "Min (µs)  | $BASELINE_MIN      | $TUNED_MIN"
echo "Avg (µs)  | $BASELINE_AVG      | $TUNED_AVG"
echo "Max (µs)  | $BASELINE_MAX      | $TUNED_MAX"
echo ""
echo "Full results in: $LOG_DIR/"
