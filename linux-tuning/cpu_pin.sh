#!/bin/bash
# CPU Pinning Script
# Author: Kasper Kanarek
# Description: Isolate CPU core and pin trading process for minimum latency

TARGET_CPU=${2:-1}
PROCESS_NAME=${1:-"bash"}

# Validate CPU exists
if [ ! -d "/sys/devices/system/cpu/cpu$TARGET_CPU" ]; then
    echo "ERROR: CPU $TARGET_CPU does not exist on this system"
    echo "Available CPUs: $(ls -d /sys/devices/system/cpu/cpu[0-9]* | grep -oP '\d+' | tr '\n' ' ')"
    exit 1
fi

echo "=== CPU Pinning Script ==="
echo "Target CPU: $TARGET_CPU"
echo "Process: $PROCESS_NAME"

# 1. Set CPU governor to performance
echo "[1/4] Setting CPU governor to performance..."
echo performance | sudo tee /sys/devices/system/cpu/cpu$TARGET_CPU/cpufreq/scaling_governor

# 2. Disable CPU idle states for target core
echo "[2/4] Disabling CPU idle states..."
sudo cpupower -c $TARGET_CPU idle-set -D 0

# 3. Pin process to target CPU
echo "[3/4] Pinning $PROCESS_NAME to CPU $TARGET_CPU..."
PID=$(pgrep -n $PROCESS_NAME)
if [ -z "$PID" ]; then
    echo "Process $PROCESS_NAME not found"
    exit 1
fi
sudo taskset -cp $TARGET_CPU $PID
echo "PID $PID pinned to CPU $TARGET_CPU"

# 4. Set FIFO scheduling
echo "[4/4] Setting FIFO scheduler for PID $PID..."
sudo chrt -f -p 80 $PID

echo ""
echo "=== CPU Pinning complete ==="
echo "Verify: taskset -cp $PID"
