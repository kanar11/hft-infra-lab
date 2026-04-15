#!/usr/bin/env python3
"""
Multicast Market Data Feed Sender

Sends timestamped market data messages over UDP multicast,
simulating a real exchange market data feed.
Wysyła oznaczone czasem wiadomości danych rynkowych przez multicast UDP,
symulując rzeczywisty kanał danych rynkowych giełdy.
"""
import socket
import time

MCAST_GROUP = '239.1.1.1'
MCAST_PORT = 5001


def main() -> None:
    """Send multicast market data in a loop until interrupted.
    Wysyła dane rynkowe multicast w pętli do przerwania.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)

    seq = 0
    try:
        while True:
            msg = f"SEQ={seq} TS={time.time_ns()}"
            sock.sendto(msg.encode(), (MCAST_GROUP, MCAST_PORT))
            print(f"Sent: {msg}")
            seq += 1
            time.sleep(0.1)
    except KeyboardInterrupt:
        print(f"\nStopped after {seq} messages.")
    finally:
        sock.close()


if __name__ == '__main__':
    main()
