#!/bin/bash
# HFT Linux Tuning Script
# Author: Kasper Kanarek
# Description: Low-latency Linux kernel tuning for HFT infrastructure
# Dostrojenie jądra Linux o niskim opóźnieniu dla infrastruktury HFT
# Compatible with Red Hat EL10 and Ubuntu 24.04

echo "=== HFT Linux Tuning Script ==="

# Prevent the kernel from moving memory pages to swap disk which causes severe latency spikes
# Uniemożliwić jądru przenoszenie stron pamięci na dysk wymiany co powoduje poważne wzrosty opóźnień
echo "[1/7] Setting swappiness to 0..."
sudo sysctl -w vm.swappiness=0

# Increase kernel network buffer sizes to prevent packet loss and dropped connections during traffic bursts
# Zwiększyć rozmiary buforów sieciowych jądra aby zapobiegać utracie pakietów i przerwaniom połączeń podczas wzrostów ruchu
echo "[2/7] Increasing network buffers..."
sudo sysctl -w net.core.rmem_max=16777216
sudo sysctl -w net.core.wmem_max=16777216
sudo sysctl -w net.core.netdev_max_backlog=250000

# Stop automatic update services and background maintenance tasks that can cause unpredictable latency
# Zatrzymać automatyczne usługi aktualizacji i zadania konserwacyjne w tle które mogą powodować nieprzewidywalne opóźnienia
echo "[3/7] Disabling background services..."
if systemctl is-active --quiet unattended-upgrades.service 2>/dev/null; then
    sudo systemctl stop unattended-upgrades.service
    echo "  Stopped unattended-upgrades"
elif systemctl is-active --quiet dnf-makecache.timer 2>/dev/null; then
    sudo systemctl stop dnf-makecache.timer
    echo "  Stopped dnf-makecache timer"
else
    echo "  No background update services running"
fi

# Reserve large memory pages to reduce translation lookaside buffer misses and memory access latency
# Zarezerwować duże strony pamięci aby zmniejszyć braki translacji w buforze lookaside i opóźnienia dostępu do pamięci
echo "[4/7] Reserving hugepages..."
sudo sysctl -w vm.nr_hugepages=512

# Optimize TCP protocol stack for low-latency trading by disabling delayed acknowledgments
# Zoptymalizować stos protokołu TCP dla handlu o niskim opóźnieniu poprzez wyłączenie opóźnionych potwierdzeń
echo "[5/7] Tuning TCP stack..."
sudo sysctl -w net.ipv4.tcp_timestamps=0
sudo sysctl -w net.ipv4.tcp_low_latency=1

# Direct all network interface interrupts to the first CPU to avoid costly context switches
# Skierować wszystkie przerwania interfejsu sieciowego do pierwszego CPU aby uniknąć kosztownych przełączeń kontekstu
echo "[6/7] Setting IRQ affinity..."
NIC=$(ls /sys/class/net | grep -E '^(eth|enp|ens)' | head -1)
if [ -n "$NIC" ]; then
    for irq in $(grep "$NIC" /proc/interrupts | awk -F: '{print $1}'); do
        echo 1 | sudo tee /proc/irq/$irq/smp_affinity > /dev/null 2>&1
    done
    echo "  Pinned $NIC IRQs to CPU0"
else
    echo "  No network interface found for IRQ pinning"
fi

# Assign this shell process to real-time FIFO priority to prevent system tasks from preempting trading logic
# Przypisać ten proces powłoki do priorytetu FIFO czasu rzeczywistego aby zapobiegać przerwaniom zadań systemowych logiki handlowej
echo "[7/7] Setting FIFO scheduler..."
sudo chrt -f -p 80 $$

echo ""
echo "=== Tuning complete ==="
echo "Verify: bash kernel-config/verify_tuning.sh"
echo "Benchmark: sudo cyclictest -l 10000 -t 1 -p 80 -i 200"
