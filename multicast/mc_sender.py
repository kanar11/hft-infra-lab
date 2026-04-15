import socket
import time

MCAST_GROUP = '239.1.1.1'
MCAST_PORT = 5001

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
