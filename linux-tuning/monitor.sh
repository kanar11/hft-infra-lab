#!/bin/bash
# HFT System Monitor
# Author: Kasper Kanarek
# Description: Real-time monitoring of latency, CPU and network interrupts

INTERVAL=1
LOG_FILE="monitor_log_$(date +%Y%m%d_%H%M%S).txt"

echo "=== HFT System Monitor ===" | tee $LOG_FILE
echo "Logging to: $LOG_FILE"
echo "Press Ctrl+C to stop"
echo ""

while true; do
    TIMESTAMP=$(date +%H:%M:%S)
    CPU=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}')
    MEM=$(free -m | awk 'NR==2{printf "Used: %sMB / Total: %sMB (%.0f%%)", $3,$2,$3*100/$2}')
    NET_IRQ=$(cat /proc/interrupts | grep -E 'eth0|ens' | awk '{sum=0; for(i=2;i<=NF-3;i++) sum+=$i; print sum}')
    CTX=$(cat /proc/stat | grep ctxt | awk '{print $2}')
    echo "[$TIMESTAMP] CPU: $CPU% | MEM: $MEM | NET IRQ: $NET_IRQ | CTX: $CTX" | tee -a $LOG_FILE
    sleep $INTERVAL
done
