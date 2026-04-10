#!/bin/bash
# HFT Linux Tuning Script
# Author: Kasper Kanarek
# Description: Low-latency Linux kernel tuning for HFT infrastructure

echo "=== HFT Linux Tuning Script ==="

# 1. Disable swap aggressiveness
echo "[1/7] Setting swappiness to 0..."
sudo sysctl -w vm.swappiness=0

# 2. Increase network buffers
echo "[2/7] Increasing network buffers..."
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216
sudo sysctl -w net.core.netdev_max_backlog=250000

# 3. Disable unattended upgrades
echo "[3/7] Disabling unattended upgrades..."
sudo systemctl stop unattended-upgrades.service

# 4. Hugepages - reserve 512 x 2MB pages (1GB)
echo "[4/7] Reserving hugepages..."
sudo sysctl -w vm.nr_hugepages=512

# 5. TCP tuning - disable Nagle algorithm and timestamps
echo "[5/7] Tuning TCP stack..."
sudo sysctl -w net.ipv4.tcp_nodelay=1
sudo sysctl -w net.ipv4.tcp_timestamps=0
sudo sysctl -w net.ipv4.tcp_low_latency=1

# 6. IRQ affinity - pin network IRQs to CPU0
echo "[6/7] Setting IRQ affinity..."
for irq in $(grep eth0 /proc/interrupts | awk -F: '{print $1}'); do
    echo 1 | sudo tee /proc/irq/$irq/smp_affinity
done

# 7. CPU scheduler - set FIFO priority for current shell
echo "[7/7] Setting FIFO scheduler..."
sudo chrt -f -p 80 $$

echo ""
echo "=== Tuning complete ==="
echo "Benchmark: sudo cyclictest -l 10000 -t 1 -p 80 -i 200"
