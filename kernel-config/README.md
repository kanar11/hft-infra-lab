# Kernel Configuration / Konfiguracja jądra

Linux kernel hardening for low-latency trading infrastructure.
*Wzmacnianie jądra Linuksa dla infrastruktury handlu o niskich opóźnieniach.*

## Optimisations Applied / Zastosowane optymalizacje
- **Hugepages**: 512 x 2MB = 1GB reserved memory
- **CPU Isolation**: isolcpus=1, nohz_full=1, rcu_nocbs=1
- **Sysctl**: swappiness=0, TCP low latency, 16MB network buffers
- **IRQ Affinity**: all interrupts pinned to CPU 0

## Files / Pliki
- `sysctl_hft.conf` — kernel parameter configuration
- `grub_params.txt` — boot parameters
- `verify_tuning.sh` — verification script

## Run
```bash
sudo cp sysctl_hft.conf /etc/sysctl.conf
sudo sysctl -p
./verify_tuning.sh
```
