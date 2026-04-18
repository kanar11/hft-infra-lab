#!/usr/bin/env python3
"""
Unit tests for Infrastructure Monitor.
Testy jednostkowe dla Monitora infrastruktury.
"""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from monitoring.infra_monitor import InfraMonitor

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
    """Test monitor initializes with default thresholds."""
    mon = InfraMonitor()
    check(mon.thresholds['cpu_percent'] == 90.0, "test_default_cpu_threshold")
    check(mon.thresholds['mem_percent'] == 85.0, "test_default_mem_threshold")
    check('context_switches_per_sec' in mon.thresholds, "test_has_ctx_switch_threshold")
    check('interrupts_on_isolated_cpu' in mon.thresholds, "test_has_irq_threshold")


def test_custom_thresholds():
    """Test monitor accepts custom thresholds."""
    custom = {'cpu_percent': 75.0, 'mem_percent': 60.0}
    mon = InfraMonitor(alert_thresholds=custom)
    check(mon.thresholds['cpu_percent'] == 75.0, "test_custom_cpu")
    check(mon.thresholds['mem_percent'] == 60.0, "test_custom_mem")


def test_cpu_stats():
    """Test CPU stats returns idle and total jiffies."""
    mon = InfraMonitor()
    try:
        stats = mon.get_cpu_stats()
        check('idle' in stats, "test_cpu_has_idle")
        check('total' in stats, "test_cpu_has_total")
        check(stats['total'] > 0, "test_cpu_total_positive")
        check(stats['idle'] >= 0, "test_cpu_idle_nonneg")
    except FileNotFoundError:
        print("  SKIP: /proc/stat not available (not Linux)")


def test_memory():
    """Test memory info returns expected fields."""
    mon = InfraMonitor()
    try:
        mem = mon.get_memory()
        check('total_mb' in mem, "test_mem_has_total")
        check('used_percent' in mem, "test_mem_has_percent")
        check('hugepages_total' in mem, "test_mem_has_hugepages")
        check(mem['total_mb'] > 0, "test_mem_total_positive")
        check(0 <= mem['used_percent'] <= 100, "test_mem_percent_range")
    except FileNotFoundError:
        print("  SKIP: /proc/meminfo not available (not Linux)")


def test_context_switches():
    """Test context switch count is positive."""
    mon = InfraMonitor()
    try:
        ctxt = mon.get_context_switches()
        check(ctxt > 0, "test_ctx_switch_positive")
    except FileNotFoundError:
        print("  SKIP: /proc/stat not available (not Linux)")


def test_collect_metrics():
    """Test collect_metrics returns all expected keys."""
    mon = InfraMonitor()
    try:
        metrics = mon.collect_metrics()
        check('timestamp' in metrics, "test_metrics_has_timestamp")
        check('memory' in metrics, "test_metrics_has_memory")
        check('context_switches' in metrics, "test_metrics_has_ctx")
        check('interrupts' in metrics, "test_metrics_has_irq")
        check('network' in metrics, "test_metrics_has_network")
    except FileNotFoundError:
        print("  SKIP: /proc not available (not Linux)")


if __name__ == '__main__':
    print("=== Infrastructure Monitor Unit Tests / Testy Monitora ===\n")
    test_init_defaults()
    test_custom_thresholds()
    test_cpu_stats()
    test_memory()
    test_context_switches()
    test_collect_metrics()
    print(f"\n{passed}/{total} tests passed")
    sys.exit(0 if passed == total else 1)
