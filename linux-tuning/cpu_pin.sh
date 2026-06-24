#!/bin/bash
# CPU Pinning Script
# Author: Kasper Kanarek
# Description: Isolate CPU core and pin trading process for minimum latency
# Isolate a CPU core and pin the trading process for minimal latency

TARGET_CPU=${2:-1}
PROCESS_NAME=${1:-"bash"}

# Check if the target CPU core exists on the system, exit if not found
# Check that the target CPU core exists on the system, exit if not found
if [ ! -d "/sys/devices/system/cpu/cpu$TARGET_CPU" ]; then
    echo "ERROR: CPU $TARGET_CPU does not exist on this system"
    echo "Available CPUs: $(ls -d /sys/devices/system/cpu/cpu[0-9]* | grep -oP '\d+' | tr '\n' ' ')"
    exit 1
fi

echo "=== CPU Pinning Script ==="
echo "Target CPU: $TARGET_CPU"
echo "Process: $PROCESS_NAME"

# Set CPU frequency scaling to performance mode for the target core to maximize speed without throttling
# Set CPU frequency scaling to performance mode for the target core to maximize speed without throttling
echo "[1/4] Setting CPU governor to performance..."
echo performance | sudo tee /sys/devices/system/cpu/cpu$TARGET_CPU/cpufreq/scaling_governor

# Prevent the CPU from entering low power idle states which cause latency spikes
# Prevent the CPU from entering low-power states, which increase latency
echo "[2/4] Disabling CPU idle states..."
sudo cpupower -c $TARGET_CPU idle-set -D 0

# Assign the trading process to run exclusively on the isolated CPU core for guaranteed resources
# Pin the trading process to run exclusively on the isolated CPU core for guaranteed resources
echo "[3/4] Pinning $PROCESS_NAME to CPU $TARGET_CPU..."
PID=$(pgrep -n $PROCESS_NAME)
if [ -z "$PID" ]; then
    echo "Process $PROCESS_NAME not found"
    exit 1
fi
sudo taskset -cp $TARGET_CPU $PID
echo "PID $PID pinned to CPU $TARGET_CPU"

# Apply real-time FIFO scheduling priority to ensure the process is not preempted by other tasks
# Apply real-time FIFO scheduling priority to ensure the process is not preempted by other tasks
echo "[4/4] Setting FIFO scheduler for PID $PID..."
sudo chrt -f -p 80 $PID

echo ""
echo "=== CPU Pinning complete ==="
echo "Verify: taskset -cp $PID"
