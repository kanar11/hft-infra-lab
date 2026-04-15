#!/bin/bash
# HFT Latency Benchmark
# Author: Kasper Kanarek
# Description: Automated cyclictest benchmark before and after tuning
# Zautomatyzowany test wydajności cyclictest przed i po dostrojeniu

DURATION=10000
PRIORITY=80
LOG_DIR="benchmark_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p $LOG_DIR

echo "=== HFT Latency Benchmark ==="

# Verify the required real-time testing tool is installed before proceeding
# Zweryfikować czy wymagane narzędzie testowania czasu rzeczywistego jest zainstalowane przed kontynuacją
if ! command -v cyclictest &> /dev/null; then
    echo "cyclictest not found. Install: sudo dnf install rt-tests"
    exit 1
fi

# Measure baseline latency before applying HFT tuning to establish a performance baseline for comparison
# Zmierzyć opóźnienie linii bazowej przed zastosowaniem dostrojenia HFT aby ustalić linię bazową wydajności do porównania
echo "[1/2] Running baseline benchmark..."
sudo cyclictest -l $DURATION -t 1 -p $PRIORITY -i 200 -q \
    > $LOG_DIR/baseline_$TIMESTAMP.txt 2>&1
echo "Baseline saved to: $LOG_DIR/baseline_$TIMESTAMP.txt"

# Extract minimum, average, and maximum latency values from baseline test results
# Wyodrębnić wartości opóźnienia minimum, średnia i maksymalna z wyników testu linii bazowej
BASELINE_MIN=$(grep "Min:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')
BASELINE_AVG=$(grep "Avg:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')
BASELINE_MAX=$(grep "Max:" $LOG_DIR/baseline_$TIMESTAMP.txt | awk '{print $2}')

# Execute all HFT tuning optimizations to reduce system latency
# Wykonać wszystkie optymalizacje dostrojenia HFT aby zmniejszyć opóźnienie systemu
echo "[2/2] Applying tuning..."
sudo ./hft_tuning.sh > /dev/null 2>&1

# Run latency benchmark again after tuning to measure improvement achieved by kernel configuration changes
# Uruchomić test wydajności opóźnienia ponownie po dostrojeniu aby zmierzyć poprawę uzyskaną przez zmiany konfiguracji jądra
echo "[3/3] Running post-tuning benchmark..."
sudo cyclictest -l $DURATION -t 1 -p $PRIORITY -i 200 -q \
    > $LOG_DIR/tuned_$TIMESTAMP.txt 2>&1
echo "Tuned saved to: $LOG_DIR/tuned_$TIMESTAMP.txt"

# Extract latency metrics from post-tuning test for comparison with baseline results
# Wyodrębnić metryki opóźnienia z testu po dostrojeniu do porównania z wynikami linii bazowej
TUNED_MIN=$(grep "Min:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')
TUNED_AVG=$(grep "Avg:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')
TUNED_MAX=$(grep "Max:" $LOG_DIR/tuned_$TIMESTAMP.txt | awk '{print $2}')

# Display comparison table showing the latency improvements achieved by HFT tuning
# Wyświetlić tabelę porównania pokazującą poprawy opóźnień uzyskane przez dostrojenie HFT
echo ""
echo "=== Results ==="
echo "Metric    | Baseline | Tuned"
echo "----------|----------|------"
echo "Min (µs)  | $BASELINE_MIN      | $TUNED_MIN"
echo "Avg (µs)  | $BASELINE_AVG      | $TUNED_AVG"
echo "Max (µs)  | $BASELINE_MAX      | $TUNED_MAX"
echo ""
echo "Full results in: $LOG_DIR/"
