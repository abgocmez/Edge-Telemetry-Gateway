# Edge Telemetry Gateway

A multi-threaded C++20 service that ingests CAN frames from one or more field
buses, stamps and sequences them, and fans them out to independent consumers
running as separate containers.

Where a supervisor daemon answers *what happens when a process dies*, this
answers **what happens when a consumer cannot keep up, when the producer outruns
the pipeline, and when a consumer disappears and comes back**.

```
vcan0 ─┐
vcan1 ─┼──► [ingest thread]──► [queue]──► [egress]──► TCP ──► probe
vcan2 ─┤     assign seq        [queue]──► [egress]──► TCP ──► recorder
LIN ───┘                       one per consumer
```

## Status

**M1 complete.** The pipeline runs end to end, in containers, under CI, with
ThreadSanitizer, against a real SocketCAN interface.

| Milestone | State |
|---|---|
| M1 — working pipeline, per-consumer mutex queues (topology A) | done |
| M2 — lock-free broadcast ring (topology B) | next |
| M3 — fan-out failure handling, gap records, split deployment | |
| M4 — measurement on the Pi | |

## Try it

Nothing but Docker is required; the default source is synthetic, so no CAN
hardware and no `vcan` are needed:

```sh
docker compose up
```

Against a real SocketCAN interface:

```sh
sudo ./scripts/setup-vcan.sh 1
ETG_SOURCE=can:vcan0 docker compose --profile vcan up
```

Building natively:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## What it does

**Ingest.** A source is a file descriptor plus a batch drain. A CAN raw socket,
the `hidraw` node of a LIN adapter, and a `timerfd`-driven synthetic generator
are all genuinely file descriptors, so the threading topology can change without
touching any source. `CanSource` reads with `recvmmsg` and enables
`SO_TIMESTAMPING` for the kernel receive timestamp and `SO_RXQ_OVFL` so frames
the kernel dropped before we saw them are counted separately from our own.

**Ordering.** The ingest thread assigns the global sequence number, because a
single bus cannot know the order across buses. The guarantee is precise:
*per-source FIFO, with an arbitrary interleaving across sources.*

**Back-pressure.** Writes are non-blocking. A consumer that falls behind fills
its socket buffer, `send` returns `EWOULDBLOCK`, the egress thread parks on
`POLLOUT`, and that consumer's queue overflows and drops — in that order, and
without touching any other consumer. `push` never blocks: a producer that waited
for space would let one slow consumer stall the entire pipeline.

**Loss.** At-most-once, drop-oldest. Every frame offered is accounted for:
`pushed == popped + dropped + size`. Consumers detect their own loss from gaps
in the sequence.

**Protocol.** A documented little-endian binary format
([docs/wire-format.md](docs/wire-format.md)), parsed field by field. The receive
buffer is never cast to a struct, even though the layouts match today. To keep
that claim honest, [`scripts/parse-capture.py`](scripts/parse-capture.py)
implements the format in another language from the specification alone, and CI
runs it against every capture.

## Measurements so far

From a GitHub Actions runner (x86_64, shared, no CPU pinning), 2000 frames/s
over `vcan0`, one generator and two consumers. These validate the plumbing;
**they are not the project's performance numbers.** Those come in M4, on a
Raspberry Pi 3 B+, with a stated method and stated limitations.

| | p50 | p99 | max |
|---|---|---|---|
| generator pacing error | 7.9 µs | 12.7 µs | 1.05 ms |
| gateway latency (ingest → consumer) | 19.2 µs | 53.0 µs | 1.21 ms |
| end-to-end (generator → consumer) | 41.7 µs | 66.8 µs | 1.22 ms |

The generator reports its own pacing error against an absolute schedule, because
a load generator that cannot prove its output was flat makes every latency
number downstream of it unfalsifiable.

Two lessons already paid for:

- **Warm-up matters.** A consumer connecting to a running gateway finds a
  backlog, and those frames were queued before it existed. Measured without
  excluding it, p99 was 345 ms — which is exactly `queue_depth / rate`, a
  property of the buffer rather than of the system. Excluding two seconds moved
  p99 to 120 µs while p50 stayed at ~44 µs.
- **A green sanitizer run proves nothing on its own.** Making one counter a
  plain `uint64_t` produces a TSan report naming both lines — while the test
  still passes all 307,364 of its assertions. Only TSan sees it.

## Layout

```
src/        gateway, generator, probe, recorder, and the shared core
docs/       wire-format.md — normative protocol specification
scripts/    vcan setup, smoke tests, independent capture parser
tests/      unit, concurrency and socket tests
SCOPE.md    locked decisions with reasons, non-goals, milestones, risks
```

## Limitations, stated

- `vcan` has no bit timing, no arbitration, no bus-off. Overload scenarios are
  synthetic and do not correspond to a 500 kbit/s bus.
- Classic CAN only; an 8-byte payload keeps the record at 40 bytes and one ring
  cell per cache line. CAN FD is a version bump, which is what the wire version
  byte is for.
- Latency figures subtract two `CLOCK_MONOTONIC` readings and are therefore
  valid only within one machine. Cross-machine measurement uses `chrony` and is
  reported separately, in M4.
- The containers share the host network namespace, because `vcan` lives in the
  host's. Separate processes over real sockets, but not yet a boundary between
  machines.
