#!/usr/bin/env python3
"""
DPDK Kernel Bypass Simulator

Simulates kernel bypass networking with poll-mode driver vs interrupt-driven I/O.
Demonstrates the latency reduction achieved by bypassing the kernel network stack.
Symuluje omijanie jądra w sieci z sterownikiem trybu ankiety w porównaniu z wejściem/wyjściem sterowanym przerwaniami.
Demonstruje zmniejszenie opóźnienia osiągnięte przez ominięcie stosu sieciowego jądra.
"""
import socket
import time
import os


class KernelBypassSimulator:
    """Simulates DPDK-style kernel bypass concepts:
    polling vs interrupt-driven I/O, zero-copy buffers, and CPU pinning.
    Symuluje koncepcje omijania jądra w stylu DPDK: sondowanie w porównaniu z wejściem/wyjściem sterowanym przerwaniami,
    bufory z kopią zerową i przypinanie CPU.
    """

    def __init__(self, cpu_core: int = 1) -> None:
        self.cpu_core = cpu_core
        self.packets_processed: int = 0
        self.total_latency_ns: int = 0

    def pin_to_core(self) -> None:
        """Pin process to isolated CPU core (like DPDK EAL).
        Przypina proces do izolowanego rdzenia CPU (jak DPDK EAL).
        """
        try:
            os.sched_setaffinity(0, {self.cpu_core})
            print(f"[DPDK-SIM] Pinned to CPU {self.cpu_core}")
        except Exception as e:
            print(f"[DPDK-SIM] CPU pin failed: {e}")

    def poll_mode_driver(self, sock: socket.socket, duration_sec: int = 5) -> int:
        """Simulate DPDK Poll Mode Driver (PMD).
        Continuously polls for packets instead of waiting for interrupts.
        Symuluje sterownik trybu ankiety DPDK (PMD). Stale sonduje pakiety zamiast czekać na przerwania.
        """
        print(f"[DPDK-SIM] Starting Poll Mode Driver for {duration_sec}s...")
        print(f"[DPDK-SIM] Mode: POLLING (no interrupts)")

        sock.setblocking(False)
        start = time.time()
        polls = 0

        while time.time() - start < duration_sec:
            polls += 1
            try:
                data, addr = sock.recvfrom(2048)
                recv_time = time.time_ns()

                # Parse timestamp from packet (Analizuje znacznik czasu z pakietu)
                try:
                    msg = data.decode('utf-8', errors='replace')
                    parts = dict(p.split('=', 1) for p in msg.split() if '=' in p)
                    send_time = int(parts.get('TS', recv_time))
                except (ValueError, UnicodeDecodeError):
                    continue

                latency = (recv_time - send_time) / 1000  # microseconds
                self.packets_processed += 1
                self.total_latency_ns += (recv_time - send_time)

                if self.packets_processed % 10 == 0:
                    print(f"  PKT #{self.packets_processed} latency={latency:.1f}us")

            except BlockingIOError:
                continue  # No packet, keep polling (busy-wait) (Brak pakietu, kontynuuj sondowanie (aktywne czekanie))

        return polls

    def print_stats(self, polls: int, duration: int) -> None:
        """Print poll mode driver performance summary.
        Drukuje podsumowanie wydajności sterownika trybu ankiety.
        """
        print(f"\n[DPDK-SIM] === Statistics ===")
        print(f"  Packets processed: {self.packets_processed}")
        print(f"  Total polls: {polls:,}")
        print(f"  Polls/sec: {polls/duration:,.0f}")
        if self.packets_processed > 0:
            avg_lat = (self.total_latency_ns / self.packets_processed) / 1000
            print(f"  Avg latency: {avg_lat:.1f} us")
        print(f"  CPU utilization: ~100% (busy polling)")


def main() -> None:
    """Run poll mode driver on multicast feed."""
    import struct

    MCAST_GROUP = '239.1.1.1'
    MCAST_PORT = 5001

    sim = KernelBypassSimulator(cpu_core=1)
    sim.pin_to_core()

    # Setup multicast socket (Skonfiguruj gniazdo multicast)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', MCAST_PORT))

    mreq = struct.pack('4sL', socket.inet_aton(MCAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    print(f"[DPDK-SIM] Listening on {MCAST_GROUP}:{MCAST_PORT}")
    print("[DPDK-SIM] Start mc_sender.py in another terminal\n")

    duration = 10
    try:
        polls = sim.poll_mode_driver(sock, duration)
        sim.print_stats(polls, duration)
    finally:
        sock.close()

if __name__ == '__main__':
    main()
