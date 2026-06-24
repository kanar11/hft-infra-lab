#!/bin/bash
# Network Latency Monitor
# Author: Kasper Kanarek
# Description: Measures network latency to trading venues and logs results
# Measure network latency to trading exchanges and log the results

TARGET=${1:-"8.8.8.8"}
COUNT=100
LOG_FILE="latency_log_$(date +%Y%m%d_%H%M%S).txt"

echo "=== Network Latency Monitor ===" | tee $LOG_FILE
echo "Target: $TARGET" | tee -a $LOG_FILE
echo "Packets: $COUNT" | tee -a $LOG_FILE
echo "Log: $LOG_FILE"
echo ""

# Measure round-trip time using ICMP packets to gauge baseline network latency
# Measure round-trip time using ICMP packets to estimate network latency
echo "[1/3] Measuring ICMP latency..." | tee -a $LOG_FILE
ping -c $COUNT $TARGET | tail -1 | tee -a $LOG_FILE

# Test TCP connection latency which is more representative of trading protocol behavior
# Test TCP connection latency, which is more representative of trading-protocol behavior
echo "" | tee -a $LOG_FILE
echo "[2/3] Measuring TCP latency..." | tee -a $LOG_FILE
if command -v hping3 &> /dev/null; then
    sudo hping3 -S -p 80 -c 10 $TARGET 2>&1 | grep "rtt" | tee -a $LOG_FILE
else
    echo "hping3 not installed. Install: sudo dnf install hping3" | tee -a $LOG_FILE
fi

# Extract min, avg, max, and jitter statistics from multiple ping samples for distribution analysis
# Extract min, mean, max and variability statistics from multiple ping samples for distribution analysis
echo "" | tee -a $LOG_FILE
echo "[3/3] Calculating jitter..." | tee -a $LOG_FILE
ping -c $COUNT $TARGET | awk -F'/' '/min\/avg\/max/{print "Min: "$4"ms | Avg: "$5"ms | Max: "$6"ms | Jitter: "$7"ms"}' | tee -a $LOG_FILE

echo "" | tee -a $LOG_FILE
echo "=== Done ===" | tee -a $LOG_FILE
echo "Results saved to: $LOG_FILE"
