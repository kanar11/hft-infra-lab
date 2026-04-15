#!/usr/bin/env python3
"""
Multicast Market Data Feed Receiver

Joins a UDP multicast group and receives timestamped market data messages
from the multicast sender, measuring basic receive throughput.
"""
import socket
import struct
import time

MCAST_GROUP = '239.1.1.1'
MCAST_PORT = 5001

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('', MCAST_PORT))

mreq = struct.pack('4sL', socket.inet_aton(MCAST_GROUP), socket.INADDR_ANY)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

print(f"Listening on {MCAST_GROUP}:{MCAST_PORT}...")

try:
    while True:
        data, addr = sock.recvfrom(1024)
        recv_time = time.time_ns()
        print(f"[{recv_time}] From {addr}: {data.decode()}")
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    sock.close()
