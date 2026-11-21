#!/usr/bin/env python3
"""Netplay TCP relay with delay and jitter (tests/netplay).

    tcp_relay.py LISTEN_PORT HOST_PORT [--delay MS] [--jitter MS] [--chunk N]
                 [--seed N] [--time S]

The guest connects to LISTEN_PORT instead of the host's ListenPort; every
byte is forwarded to the host on HOST_PORT and back. Each chunk read from a
socket (at most --chunk bytes, so one write can arrive in pieces) is held
for delay +- jitter ms. TCP keeps its order, so a chunk is never delivered
before the chunk before it in the same direction. Ikemen's TCP part (sync
settings, seed, pre-match time, delay-netplay input frames, the stop marker)
then sees a slow, uneven link. Several connections in a row are relayed
(one session after another). Prints the byte counts when it ends (after
--time seconds, or on SIGTERM/SIGINT).
"""
import argparse, heapq, random, select, signal, socket, time

ap = argparse.ArgumentParser()
ap.add_argument("listen_port", type=int)
ap.add_argument("host_port", type=int)
ap.add_argument("--delay", type=float, default=0)
ap.add_argument("--jitter", type=float, default=0)
ap.add_argument("--chunk", type=int, default=4096)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--time", type=float, default=600)
o = ap.parse_args()

rnd = random.Random(o.seed)
lo = "127.0.0.1"
ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind((lo, o.listen_port))
ls.listen(4)
running = True
stats = {"g2h": 0, "h2g": 0, "conns": 0}


def stop(*_):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

pairs = {}   # socket -> (peer socket, direction)
last_due = {}  # destination socket -> due time of its last queued chunk
q = []       # (due, seq, dest, data); data b"" = close
seq = 0
t0 = time.monotonic()


def close_pair(s):
    p = pairs.pop(s, None)
    if p:
        pairs.pop(p[0], None)
    for x in (s, p[0] if p else None):
        if x:
            try:
                x.close()
            except OSError:
                pass


while running and time.monotonic() - t0 < o.time:
    now = time.monotonic()
    while q and q[0][0] <= now:
        _, _, dest, data = heapq.heappop(q)
        if dest.fileno() < 0:
            continue
        try:
            if data:
                dest.sendall(data)
            else:
                dest.shutdown(socket.SHUT_WR)
        except OSError:
            close_pair(dest)
    timeout = max(0.0, min(0.05, q[0][0] - now)) if q else 0.05
    rl = [ls] + list(pairs.keys())
    r, _, _ = select.select(rl, [], [], timeout)
    for s in r:
        if s is ls:
            g, _ = ls.accept()
            h = socket.create_connection((lo, o.host_port))
            for x in (g, h):
                x.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            pairs[g] = (h, "g2h")
            pairs[h] = (g, "h2g")
            stats["conns"] += 1
            continue
        if s not in pairs:
            continue
        dest, k = pairs[s]
        try:
            data = s.recv(o.chunk)
        except OSError:
            data = b""
        d = (o.delay + rnd.uniform(-o.jitter, o.jitter)) / 1000.0
        due = max(time.monotonic() + max(0.0, d), last_due.get(dest, 0.0))
        last_due[dest] = due
        seq += 1
        heapq.heappush(q, (due, seq, dest, data))
        stats[k] += len(data)
        if not data:  # peer closed: forward the close after the queued data
            pairs.pop(s, None)
print("tcp relay: conns %d, guest->host %d bytes, host->guest %d bytes" % (stats["conns"], stats["g2h"], stats["h2g"]))
