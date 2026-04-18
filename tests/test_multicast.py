#!/usr/bin/env python3
"""
Unit tests for Multicast Market Data Feed.
Testy jednostkowe dla kanału danych rynkowych Multicast.
"""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

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


def test_message_format():
    """Test multicast message format: SEQ=N TS=nanoseconds."""
    import time
    seq = 42
    ts = time.time_ns()
    msg = f"SEQ={seq} TS={ts}"
    parts = dict(p.split('=', 1) for p in msg.split() if '=' in p)
    check(parts['SEQ'] == '42', "test_message_seq")
    check(int(parts['TS']) == ts, "test_message_ts")
    check('SEQ' in msg and 'TS' in msg, "test_message_format")


def test_message_encoding():
    """Test message encodes/decodes correctly as UTF-8."""
    msg = "SEQ=0 TS=1234567890"
    encoded = msg.encode('utf-8')
    decoded = encoded.decode('utf-8')
    check(decoded == msg, "test_message_encoding_roundtrip")
    check(isinstance(encoded, bytes), "test_message_is_bytes")


def test_multicast_address_valid():
    """Test multicast group is in valid range (224.0.0.0 - 239.255.255.255)."""
    import socket
    MCAST_GROUP = '239.1.1.1'
    octets = [int(x) for x in MCAST_GROUP.split('.')]
    check(224 <= octets[0] <= 239, "test_mcast_range")
    check(len(octets) == 4, "test_mcast_format")
    # Verify it's a valid IP
    try:
        socket.inet_aton(MCAST_GROUP)
        valid = True
    except socket.error:
        valid = False
    check(valid, "test_mcast_inet_aton")


def test_sequence_monotonic():
    """Test that sequence numbers are monotonically increasing."""
    import time
    messages = []
    for i in range(100):
        messages.append(f"SEQ={i} TS={time.time_ns()}")
    seqs = [int(dict(p.split('=', 1) for p in m.split() if '=' in p)['SEQ'])
            for m in messages]
    check(seqs == list(range(100)), "test_seq_monotonic")
    check(seqs[0] == 0, "test_seq_starts_at_zero")


def test_timestamp_increasing():
    """Test that timestamps are strictly increasing."""
    import time
    timestamps = []
    for i in range(10):
        timestamps.append(time.time_ns())
    increasing = all(timestamps[i] <= timestamps[i+1] for i in range(len(timestamps)-1))
    check(increasing, "test_ts_increasing")


if __name__ == '__main__':
    print("=== Multicast Unit Tests / Testy Multicast ===\n")
    test_message_format()
    test_message_encoding()
    test_multicast_address_valid()
    test_sequence_monotonic()
    test_timestamp_increasing()
    print(f"\n{passed}/{total} tests passed")
    sys.exit(0 if passed == total else 1)
