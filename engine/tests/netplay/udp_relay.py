#!/usr/bin/env python3
"""Rollback UDP relay with delay, jitter and loss (tests/netplay).

    udp_relay.py HOST_PORT GUEST_PORT RELAY_A RELAY_B [--delay MS] [--jitter MS]
                 [--loss PCT] [--seed N] [--time S]

The host (rollback UDP on HOST_PORT) is told the guest is at RELAY_A
(CVF_NET_PEER_UDP), the guest (GUEST_PORT) that the host is at RELAY_B.
Packets the host sends to RELAY_A leave from RELAY_B to the guest, and the
other way round, so each side sees its peer at the address it expects.
Every packet is held for delay +- jitter ms, or dropped with probability
loss %. Prints the packet counts when it ends (after --time seconds, or on
SIGTERM/SIGINT).
"""
import argparse, heapq, random, select, signal, socket, sys, time

ap = argparse.ArgumentParser()
for a in ("host_port", "guest_port", "relay_a", "relay_b"):
    ap.add_argument(a, type=int)
ap.add_argument("--delay", type=float, default=0)
ap.add_argument("--jitter", type=float, default=0)
ap.add_argument("--loss", type=float, default=0)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--time", type=float, default=600)
o = ap.parse_args()

rnd = random.Random(o.seed)
lo = "127.0.0.1"
sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sa.bind((lo, o.relay_a))
sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sb.bind((lo, o.relay_b))
q = []  # (due, seq, socket, data, dest)
seq = 0
stats = {"h2g": 0, "g2h": 0, "dropped": 0}
running = True


def stop(*_):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
end = time.monotonic() + o.time
while running and time.monotonic() < end:
    now = time.monotonic()
    while q and q[0][0] <= now:
        _, _, s, data, dest = heapq.heappop(q)
        try:
            s.sendto(data, dest)
        except OSError:
            pass
    timeout = max(0.0, min(0.05, q[0][0] - now)) if q else 0.05
    r, _, _ = select.select([sa, sb], [], [], timeout)
    for s in r:
        try:
            data, _ = s.recvfrom(65536)
        except OSError:
            continue
        if s is sa:  # host -> guest, sent from relay B
            out, dest, k = sb, (lo, o.guest_port), "h2g"
        else:        # guest -> host, sent from relay A
            out, dest, k = sa, (lo, o.host_port), "g2h"
        stats[k] += 1
        if rnd.random() * 100 < o.loss:
            stats["dropped"] += 1
            continue
        d = max(0.0, o.delay + rnd.uniform(-o.jitter, o.jitter)) / 1000
        seq += 1
        heapq.heappush(q, (time.monotonic() + d, seq, out, data, dest))
print("relay: host->guest %d, guest->host %d, dropped %d" % (stats["h2g"], stats["g2h"], stats["dropped"]))
sys.stdout.flush()
