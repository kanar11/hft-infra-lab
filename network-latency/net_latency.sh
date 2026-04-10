#!/bin/bash
# Network Latency Monitor
# Author: Kasper Kanarek
# Description: Measures network latency to trading venues and logs results

TARGET=${1:-"8.8.8.8"}
COUNT=100
LOG_FILE="latency_log_$(date +%Y%m%d_%H%M%S).txt"

echo "=== Network Latency Monitor ===" | tee $LOG_FILE
echo "Target: $TARGET" | tee -a $LOG_FILE
echo "Packets: $COUNT" | tee -a $LOG_FILE
echo "Log: $LOG_FILE"
echo ""

# 1. Basic ping latency
echo "[1/3] Measuring ICMP latency..." | tee -a $LOG_FILE
ping -c $COUNT $TARGET | tail -1 | tee -a $LOG_FILE

# 2. TCP latency with hping3
echo "" | tee -a $LOG_FILE
echo "[2/3] Measuring TCP latency..." | tee -a $LOG_FILE
if command -v hping3 &> /dev/null; then
    sudo hping3 -S -p 80 -c 10 $TARGET 2>&1 | grep "rtt" | tee -a $LOG_FILE
else
    echo "hping3 not installed. Install: sudo apt install hping3" | tee -a $LOG_FILE
fi

# 3. Jitter calculation
echo "" | tee -a $LOG_FILE
echo "[3/3] Calculating jitter..." | tee -a $LOG_FILE
ping -c $COUNT $TARGET | awk -F'/' 'END{print "Min: "$1"ms | Avg: "$2"ms | Max: "$3"ms | Jitter: "$4"ms"}' | tee -a $LOG_FILE

echo "" | tee -a $LOG_FILE
echo "=== Done ===" | tee -a $LOG_FILE
echo "Results saved to: $LOG_FILE"
