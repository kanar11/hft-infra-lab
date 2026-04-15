import time
import os
import json
from datetime import datetime

class InfraMonitor:
    def __init__(self, alert_thresholds=None):
        self.thresholds = alert_thresholds or {
            'cpu_percent': 90.0,
            'mem_percent': 85.0,
            'context_switches_per_sec': 50000,
            'interrupts_on_isolated_cpu': 100
        }
        self.metrics_log = []

    def get_cpu_stats(self):
        with open('/proc/stat') as f:
            line = f.readline()
            parts = line.split()
            idle = int(parts[4])
            total = sum(int(p) for p in parts[1:])
            return {'idle': idle, 'total': total}

    def get_memory(self):
        info = {}
        with open('/proc/meminfo') as f:
            for line in f:
                parts = line.split()
                key = parts[0].rstrip(':')
                info[key] = int(parts[1])
        total = info['MemTotal']
        available = info['MemAvailable']
        hugepages_total = info.get('HugePages_Total', 0)
        hugepages_free = info.get('HugePages_Free', 0)
        return {
            'total_mb': total // 1024,
            'used_percent': round((1 - available / total) * 100, 1),
            'hugepages_total': hugepages_total,
            'hugepages_used': hugepages_total - hugepages_free
        }

    def get_context_switches(self):
        with open('/proc/stat') as f:
            for line in f:
                if line.startswith('ctxt'):
                    return int(line.split()[1])
        return 0

    def get_interrupts_per_cpu(self):
        counts = {}
        with open('/proc/interrupts') as f:
            header = f.readline().split()
            num_cpus = len(header)
            for line in f:
                parts = line.split()
                if len(parts) > num_cpus:
                    for i in range(num_cpus):
                        cpu = f"CPU{i}"
                        counts[cpu] = counts.get(cpu, 0) + int(parts[i + 1])
        return counts

    def get_network_stats(self):
        with open('/proc/net/dev') as f:
            f.readline()
            f.readline()
            for line in f:
                iface = line.split(':')[0].strip()
                if iface != 'lo' and iface != '':
                    parts = line.split()
                    return {
                        'interface': iface,
                        'rx_bytes': int(parts[1]),
                        'rx_packets': int(parts[2]),
                        'tx_bytes': int(parts[9]),
                        'tx_packets': int(parts[10])
                    }
        return {}

    def check_isolated_cpu(self):
        try:
            with open('/sys/devices/system/cpu/isolated') as f:
                isolated = f.read().strip()
            return isolated
        except FileNotFoundError:
            return ''

    def collect_metrics(self):
        return {
            'timestamp': datetime.now().isoformat(),
            'memory': self.get_memory(),
            'context_switches': self.get_context_switches(),
            'interrupts': self.get_interrupts_per_cpu(),
            'network': self.get_network_stats(),
            'isolated_cpus': self.check_isolated_cpu()
        }

    def check_alerts(self, metrics):
        alerts = []
        mem = metrics['memory']['used_percent']
        if mem > self.thresholds['mem_percent']:
            alerts.append(f"ALERT: Memory {mem}% > {self.thresholds['mem_percent']}%")

        iso_cpu = metrics['isolated_cpus']
        if iso_cpu:
            # Parse CPU ranges like "1", "1-3", "1,3"
            cpu_ids = []
            for part in iso_cpu.split(','):
                if '-' in part:
                    start, end = part.split('-', 1)
                    cpu_ids.extend(range(int(start), int(end) + 1))
                else:
                    cpu_ids.append(int(part))
            for cpu_id in cpu_ids:
                cpu_key = f"CPU{cpu_id}"
                irqs = metrics['interrupts'].get(cpu_key, 0)
                if irqs > self.thresholds['interrupts_on_isolated_cpu']:
                    alerts.append(f"ALERT: {irqs} interrupts on isolated {cpu_key}")

        return alerts

    def run(self, duration=30, interval=5):
        print("=== HFT Infrastructure Monitor ===")
        print(f"Monitoring for {duration}s, interval {interval}s\n")

        start = time.time()
        prev_ctx = self.get_context_switches()
        prev_net = self.get_network_stats()
        prev_time = start

        while time.time() - start < duration:
            metrics = self.collect_metrics()
            now = time.time()
            dt = now - prev_time

            # Context switches per second
            ctx = metrics['context_switches']
            ctx_per_sec = int((ctx - prev_ctx) / dt)
            prev_ctx = ctx

            # Network throughput
            net = metrics['network']
            rx_bps = int((net['rx_bytes'] - prev_net.get('rx_bytes', 0)) / dt)
            prev_net = net
            prev_time = now

            mem = metrics['memory']
            print(f"[{metrics['timestamp'][11:19]}]")
            print(f"  MEM: {mem['used_percent']}% | HugePages: {mem['hugepages_used']}/{mem['hugepages_total']}")
            print(f"  CTX switches/s: {ctx_per_sec:,}")
            print(f"  NET rx: {rx_bps:,} B/s | pkts: {net['rx_packets']:,}")
            print(f"  Isolated CPUs: {metrics['isolated_cpus']}")

            alerts = self.check_alerts(metrics)
            for a in alerts:
                print(f"  *** {a} ***")
            print()

            self.metrics_log.append(metrics)
            time.sleep(interval)

        # Save log
        logfile = f"monitor_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        with open(logfile, 'w') as f:
            json.dump(self.metrics_log, f, indent=2)
        print(f"Log saved to {logfile}")


if __name__ == '__main__':
    monitor = InfraMonitor()
    monitor.run(duration=30, interval=5)
