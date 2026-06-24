#!/bin/bash
# HFT System Monitor
# Author: Kasper Kanarek
# Description: Real-time monitoring of latency, CPU and network interrupts
# Real-time monitoring of latency, CPU and network interrupts
# Uses /proc directly instead of expensive tools like top
# Uses /proc directly instead of expensive tools like top

INTERVAL=1
LOG_FILE="monitor_log_$(date +%Y%m%d_%H%M%S).txt"

echo "=== HFT System Monitor ===" | tee $LOG_FILE
echo "Logging to: $LOG_FILE"
echo "Press Ctrl+C to stop"
echo ""

# Parse CPU usage statistics directly from /proc filesystem avoiding overhead of external tools
# Analyze CPU usage statistics directly from the /proc filesystem, avoiding the overhead of external tools
read_cpu() {
    local line=$(head -1 /proc/stat)
    local -a fields=($line)
    local idle=${fields[4]}
    local total=0
    for i in "${fields[@]:1}"; do
        total=$((total + i))
    done
    echo "$idle $total"
}

PREV_CPU=($(read_cpu))
PREV_CTX=$(awk '/^ctxt/ {print $2}' /proc/stat)

while true; do
    TIMESTAMP=$(date +%H:%M:%S)

    # Calculate current CPU usage percentage from idle time delta without invoking system tools
    # Compute the current CPU usage percentage from the idle-time delta without invoking system tools
    CURR_CPU=($(read_cpu))
    IDLE_DELTA=$((${CURR_CPU[0]} - ${PREV_CPU[0]}))
    TOTAL_DELTA=$((${CURR_CPU[1]} - ${PREV_CPU[1]}))
    if [ $TOTAL_DELTA -gt 0 ]; then
        CPU_PCT=$(( (TOTAL_DELTA - IDLE_DELTA) * 100 / TOTAL_DELTA ))
    else
        CPU_PCT=0
    fi
    PREV_CPU=("${CURR_CPU[@]}")

    # Get current memory usage statistics from free command
    # Get current memory usage statistics from the free command
    MEM=$(free -m | awk 'NR==2{printf "Used: %sMB / Total: %sMB (%.0f%%)", $3,$2,$3*100/$2}')

    # Count total network interrupts for Ethernet, ENP, and ENS interfaces to detect traffic spikes
    # Count total network interrupts for Ethernet, ENP and ENS interfaces to detect traffic spikes
    NET_IRQ=$(awk '/eth|ens|enp/ {for(i=2;i<=NF-3;i++) sum+=$i} END{print sum+0}' /proc/interrupts)

    # Measure context switch rate per second to identify task scheduling overhead
    # Measure the context-switch rate per second to identify task-scheduling overhead
    CTX=$(awk '/^ctxt/ {print $2}' /proc/stat)
    CTX_DELTA=$((CTX - PREV_CTX))
    PREV_CTX=$CTX

    echo "[$TIMESTAMP] CPU: ${CPU_PCT}% | MEM: $MEM | NET IRQ: $NET_IRQ | CTX/s: $CTX_DELTA" | tee -a $LOG_FILE
    sleep $INTERVAL
done
