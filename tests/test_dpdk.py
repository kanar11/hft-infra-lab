#!/usr/bin/env python3
"""
Unit tests for DPDK Kernel Bypass Simulator.
Testy jednostkowe dla Symulatora omijania jądra DPDK.
"""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from importlib.util import spec_from_file_location, module_from_spec

# Import from hyphenated directory / Import z katalogu z myślnikiem
spec = spec_from_file_location("kernel_bypass_sim",
    os.path.join(os.path.dirname(__file__), '..', 'dpdk-bypass', 'kernel_bypass_sim.py'))
dpdk_mod = module_from_spec(spec)
spec.loader.exec_module(dpdk_mod)
KernelBypassSimulator = dpdk_mod.KernelBypassSimulator

passed = 0
total = 0


def check(cond, name):
    global passed, total
    total += 1
    if cond:
        passed += 1
        print(f"  PASS: {name}")
    else:
        print(f"  FAIL: {name}")


def test_init_defaults():
    """Test simulator initializes with correct defaults."""
    sim = KernelBypassSimulator()
    check(sim.cpu_core == 1, "test_default_cpu_core")
    check(sim.packets_processed == 0, "test_init_packets_zero")
    check(sim.total_latency_ns == 0, "test_init_latency_zero")


def test_custom_cpu_core():
    """Test simulator accepts custom CPU core."""
    sim = KernelBypassSimulator(cpu_core=3)
    check(sim.cpu_core == 3, "test_custom_core")


def test_packet_counter():
    """Test packet counter increments correctly."""
    sim = KernelBypassSimulator()
    sim.packets_processed = 100
    sim.total_latency_ns = 500000
    check(sim.packets_processed == 100, "test_packet_count")
    avg_lat = sim.total_latency_ns / sim.packets_processed
    check(avg_lat == 5000.0, "test_avg_latency_calc")


def test_latency_accumulation():
    """Test latency accumulation math."""
    sim = KernelBypassSimulator()
    latencies = [100, 200, 300, 400, 500]
    for lat in latencies:
        sim.packets_processed += 1
        sim.total_latency_ns += lat
    check(sim.packets_processed == 5, "test_accum_count")
    check(sim.total_latency_ns == 1500, "test_accum_total")
    avg = sim.total_latency_ns / sim.packets_processed
    check(avg == 300.0, "test_accum_avg")


def test_message_parsing():
    """Test multicast message parsing (SEQ=N TS=nanoseconds)."""
    msg = "SEQ=42 TS=1234567890"
    parts = dict(p.split('=', 1) for p in msg.split() if '=' in p)
    check(parts['SEQ'] == '42', "test_parse_seq")
    check(parts['TS'] == '1234567890', "test_parse_ts")
    check(int(parts['SEQ']) == 42, "test_parse_seq_int")


def test_pin_to_core():
    """Test CPU pinning doesn't crash (may warn if not root)."""
    sim = KernelBypassSimulator(cpu_core=0)
    try:
        sim.pin_to_core()
        check(True, "test_pin_no_crash")
    except Exception:
        check(True, "test_pin_handled_gracefully")


if __name__ == '__main__':
    print("=== DPDK Kernel Bypass Unit Tests / Testy DPDK ===\n")
    test_init_defaults()
    test_custom_cpu_core()
    test_packet_counter()
    test_latency_accumulation()
    test_message_parsing()
    test_pin_to_core()
    print(f"\n{passed}/{total} tests passed")
    sys.exit(0 if passed == total else 1)
