#!/usr/bin/env python3
"""
Multicast Market Data Feed Receiver

Joins a UDP multicast group and receives timestamped market data messages
from the multicast sender, measuring basic receive throughput.
Dołącza do grupy multicast UDP i odbiera oznaczone czasem wiadomości danych rynkowych
od nadawcy multicast, mierząc podstawową przepustowość odboru.
"""
import socket
import struct
import time
import logging
import sys
import os

logger = logging.getLogger('multicast.receiver')

MCAST_GROUP = '239.1.1.1'
MCAST_PORT = 5001


def main() -> None:
    """Join multicast group and print received messages.
    Dołącza do grupy multicast i drukuje odebrane wiadomości.
    """
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
    from config_loader import setup_logging
    setup_logging()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', MCAST_PORT))

    mreq = struct.pack('4sL', socket.inet_aton(MCAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    logger.info(f"Listening on {MCAST_GROUP}:{MCAST_PORT}...")

    try:
        while True:
            data, addr = sock.recvfrom(1024)
            recv_time = time.time_ns()
            logger.info(f"[{recv_time}] From {addr}: {data.decode()}")
    except KeyboardInterrupt:
        logger.info("Stopped.")
    finally:
        sock.close()


if __name__ == '__main__':
    main()
