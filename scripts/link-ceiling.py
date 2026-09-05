#!/usr/bin/env python3
"""Measure the TCP throughput and round-trip time of the link between two hosts.

This has to be done before any cross-machine throughput figure is published,
because otherwise the number reported is the network's and not the gateway's.
The Pi 3 B+ makes that especially easy to get wrong: its Ethernet is attached
over USB 2.0 and shares that bus with everything else, so its realistic ceiling
is nowhere near the gigabit the interface advertises.

Written here rather than shelling out to iperf3 so the measurement is committed
alongside everything it is used to qualify, and so it runs wherever python3
does - iperf3 is not installed on both ends of this particular pair.

    # on the machine under test
    scripts/link-ceiling.py serve

    # on the other one
    scripts/link-ceiling.py measure --host 192.168.1.103

Reports throughput in both directions and the round-trip time distribution. The
RTT half matters as much as the bandwidth: a one-way latency measurement across
these two hosts cannot resolve anything smaller than it.
"""

import argparse
import socket
import statistics
import struct
import sys
import time

PORT = 5299
CHUNK = 1 << 16
MAGIC = b"ETGL"


def serve(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", port))
    srv.listen(1)
    print(f"listening on {port}", flush=True)

    while True:
        conn, addr = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"connection from {addr[0]}", flush=True)
        try:
            handle(conn)
        except (ConnectionError, OSError) as exc:
            print(f"  {exc}", flush=True)
        finally:
            conn.close()


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return bytes(buf)


def handle(conn):
    header = recv_exact(conn, 12)
    magic, mode, size = struct.unpack("!4sII", header)
    if magic != MAGIC:
        raise ConnectionError(f"bad magic {magic!r}")

    if mode == 0:  # peer uploads to us; drain and count
        got = 0
        while got < size:
            chunk = conn.recv(min(CHUNK, size - got))
            if not chunk:
                break
            got += len(chunk)
        conn.sendall(struct.pack("!Q", got))

    elif mode == 1:  # peer wants to download from us
        payload = bytes(CHUNK)
        sent = 0
        while sent < size:
            n = conn.send(payload[: min(CHUNK, size - sent)])
            sent += n

    elif mode == 2:  # round-trip ping: echo whatever arrives
        while True:
            data = conn.recv(64)
            if not data:
                return
            conn.sendall(data)


def connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def measure_upload(host, port, size):
    s = connect(host, port)
    s.sendall(struct.pack("!4sII", MAGIC, 0, size))
    payload = bytes(CHUNK)
    t0 = time.perf_counter()
    sent = 0
    while sent < size:
        sent += s.send(payload[: min(CHUNK, size - sent)])
    got = struct.unpack("!Q", recv_exact(s, 8))[0]
    elapsed = time.perf_counter() - t0
    s.close()
    return got * 8 / elapsed / 1e6


def measure_download(host, port, size):
    s = connect(host, port)
    s.sendall(struct.pack("!4sII", MAGIC, 1, size))
    t0 = time.perf_counter()
    got = 0
    while got < size:
        chunk = s.recv(CHUNK)
        if not chunk:
            break
        got += len(chunk)
    elapsed = time.perf_counter() - t0
    s.close()
    return got * 8 / elapsed / 1e6


def measure_rtt(host, port, count):
    s = connect(host, port)
    s.sendall(struct.pack("!4sII", MAGIC, 2, 0))
    payload = b"x" * 32
    samples = []
    for _ in range(count):
        t0 = time.perf_counter()
        s.sendall(payload)
        recv_exact(s, len(payload))
        samples.append((time.perf_counter() - t0) * 1e6)
    s.close()
    samples.sort()
    return samples


def pct(sorted_samples, q):
    i = int(q * len(sorted_samples))
    return sorted_samples[min(i, len(sorted_samples) - 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=["serve", "measure"])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--mb", type=int, default=64, help="transfer size per direction")
    ap.add_argument("--pings", type=int, default=500)
    args = ap.parse_args()

    if args.mode == "serve":
        serve(args.port)
        return 0

    size = args.mb * 1024 * 1024
    up = measure_upload(args.host, args.port, size)
    down = measure_download(args.host, args.port, size)
    rtt = measure_rtt(args.host, args.port, args.pings)

    print(f"host            {args.host}")
    print(f"transfer        {args.mb} MB each way")
    print()
    print(f"upload          {up:.1f} Mbit/s")
    print(f"download        {down:.1f} Mbit/s")
    print()
    print(f"rtt  n={len(rtt)}")
    print(f"  min   {rtt[0]:.0f} us")
    print(f"  p50   {pct(rtt, 0.50):.0f} us")
    print(f"  p90   {pct(rtt, 0.90):.0f} us")
    print(f"  p99   {pct(rtt, 0.99):.0f} us")
    print(f"  max   {rtt[-1]:.0f} us")
    print(f"  mean  {statistics.fmean(rtt):.0f} us")
    print()
    print("One-way latency across this pair cannot resolve anything smaller than")
    print(f"about {pct(rtt, 0.50) / 2:.0f} us, which is half the median round trip.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
