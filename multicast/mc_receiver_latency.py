#!/usr/bin/env python3
"""
Multicast Latency Measurement Receiver

Receives multicast market data and measures end-to-end latency
by comparing send and receive timestamps for each message.
"""
import socket
import struct
import time

MCAST_GROUP = '239.1.1.1'
MCAST_PORT = 5001


def main() -> None:
    """Join multicast group and measure per-message latency."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', MCAST_PORT))

    mreq = struct.pack('4sL', socket.inet_aton(MCAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    print("Measuring multicast latency...")
    print(f"{'SEQ':<8} {'Latency (us)':<15}")
    print("-" * 25)

    errors = 0
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            recv_time = time.time_ns()
            msg = data.decode()
            parts = dict(p.split('=') for p in msg.split() if '=' in p)

            if 'TS' not in parts or 'SEQ' not in parts:
                errors += 1
                if errors <= 10:
                    print(f"  WARN: malformed message (missing TS/SEQ): {msg[:60]}")
                continue

            send_time = int(parts['TS'])
            latency_us = (recv_time - send_time) / 1000
            print(f"{parts['SEQ']:<8} {latency_us:<15.2f}")
        except (ValueError, UnicodeDecodeError) as e:
            errors += 1
            if errors <= 10:
                print(f"  WARN: parse error: {e}")
        except KeyboardInterrupt:
            print(f"\nStopped. Total parse errors: {errors}")
            break
    sock.close()


if __name__ == '__main__':
    main()
