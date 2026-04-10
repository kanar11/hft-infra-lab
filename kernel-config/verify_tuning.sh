#!/bin/bash
echo "=== HFT Infra Lab - System Verification ==="
echo ""
echo "--- Hugepages ---"
grep HugePages_Total /proc/meminfo
grep HugePages_Free /proc/meminfo
echo ""
echo "--- CPU Isolation ---"
cat /sys/devices/system/cpu/isolated
echo ""
echo "--- Kernel Boot Params ---"
cat /proc/cmdline
echo ""
echo "--- Swappiness ---"
cat /proc/sys/vm/swappiness
echo ""
echo "--- Network Buffers ---"
sysctl net.core.rmem_max
sysctl net.core.wmem_max
echo ""
echo "--- NUMA ---"
numactl --hardware | head -4
echo ""
echo "=== Done ==="
