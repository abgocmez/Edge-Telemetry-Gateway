# Edge Telemetry Gateway — Agreed Scope

Status: scope locked, no implementation started.
Companion project: failsafe supervisor daemon (process death). This project: pipeline overload,
consumer slowness, and consumer loss.

## Purpose

Close two gaps against a target role on a failsafe industrial-edge runtime platform:
multi-threading/concurrency, and distributed/microservice architecture.
Narrow and measured beats broad and half-finished.

## Locked decisions

| Decision | Choice | Reason |
|---|---|---|
| Frame model | Raw passthrough, no decoding | No DBC parser, no config schema, no wire versioning. Bottleneck becomes syscalls, so the performance story is batching and syscall amortisation. |
| Sources | Multi-bus: vcan0..vcan3, LIN/HID later | Makes multi-producer honest rather than contrived. Realistic for an edge gateway. |
| Core structure | Broadcast ring, per-cell seqlock, CAS producer claim | Readers never block writers by construction. Overrun is detected structurally via sequence gap. Scales a primitive already proven in the supervisor. |
| Thread pool | Dropped | No per-frame CPU work to parallelise. On 4 Pi cores a pool would add contention and break source ordering. |
| Delivery guarantee | At-most-once, drop-oldest, loss counted and reported to the consumer | The only guarantee an overwriting ring can offer. More defensible than claiming more. |
| Ordering guarantee | Per-source FIFO. Cross-source interleaving is arbitrary (ticket-claim order, not bus arrival time). | Falls out of fetch_add ticketing. State precisely; do not claim total order. |
| Transport | TCP, one connection per consumer, length-prefixed fixed-layout binary, non-blocking + epoll | Unambiguous network boundary. Real back-pressure via EWOULDBLOCK. Real disconnect detection. TCP_NODELAY required. |
| Deployment | Gateway + recorder + cadence monitor on Pi 3 B+; probe on both Pi and dev machine over LAN | Split makes "distributed" unambiguous and gives local-vs-network comparison on identical consumer code. |
| Consumers | All C++ | Mitigated by a byte-level wire spec, parse-from-bytes (no shared-struct casting), and a golden byte-array test on both ends. |
| Sequence numbers | Present from day one | Enables gap markers now and bounded replay later without a format change. |

## Consumers

- **Recorder** (Pi) — writes to disk. SD card I/O is a genuine slow consumer; no simulation needed.
- **Probe** (Pi + dev machine) — rate and latency histogram. The measurement instrument.
- **Cadence monitor** (Pi) — per-ID inter-arrival checking, `--stall-ms` knob doubles as the overload lever.
  Carries the argument: without gap markers it reports false anomalies; with them it is correct.

## Non-goals (explicitly cut)

Consensus / Raft / leader election / quorum. Multiple gateway nodes or gateway HA. Service discovery.
External broker (MQTT/Kafka/NATS/ZeroMQ) as core fan-out. Serialization framework (protobuf/capnp/flatbuffers).
Durability, WAL, replication. Kubernetes. TLS/auth. Web UI. Exactly-once.

## Milestones

Ordered so each is independently shippable. The cut line can land anywhere.

- **M1 — visibly runs.** vcan0, own timerfd-paced generator as a separate process publishing its
  own pacing jitter histogram. One source thread. **Two per-consumer mutex + condvar bounded queues**
  (probe + recorder), fan-out thread copies into both. TCP egress. CMake + Ninja, docker compose up,
  CI build + smoke test.
  This is topology A (N copies, locks) in full — a complete, defensible alternative to M2 topology B
  (1 copy, lock-free broadcast), not a stub. If M2 fails, M1 still stands as an architecture, and M4
  compares the two designs rather than just mutex-vs-atomics.
  The M1 recorder is deliberately dumb: append and count. No rotation, no gap records, no fsync
  policy — all of that is M3.
- **M2 — the ring.** Multi-bus, M source threads. Broadcast ring behind M1's interface.
  TSan in CI. Deliberately-broken relaxed-instead-of-release variant. Randomized stress on
  x86_64 and aarch64. Padded vs unpadded throughput.
- **M3 — fan-out and failure.** Three consumers, independent cursors, reconnect with backoff,
  gap markers on the wire, drop counters by (consumer, reason), SO_RXQ_OVFL for kernel-side drops.
  Split deployment. docker kill and recover.
- **M4 — the numbers.** Latency distribution, sustained throughput, drop rate vs offered load,
  recovery time after kill, batch-size sweep across the latency/throughput frontier.
  Committed scripts, stated method, stated limitations.
- **M5 — stretches, in order.** SCHED_FIFO/isolcpus comparison, thermal trace,
  LIN/HID source, bounded replay. Stop when time runs out.

## M1 design decisions

### Source abstraction boundary

The gateway owns the threads; a source is an fd plus a batch drain.

- `fd()` for epoll/poll, `drain(out, max)` for a non-blocking batch read, `id()` for per-source
  FIFO and per-source stats, plus counters (frames, kernel drops via SO_RXQ_OVFL, errors).
- Chosen because it is not an invented abstraction: a CAN raw socket, hidraw for the LIN hardware,
  and a timerfd-driven synthetic generator are all genuinely fds.
- Decouples threading topology from source implementations: thread-per-source vs one epoll thread
  over all sources becomes a swap, and a measurable comparison in M4 on 4 Pi cores.
- Solves ingest shutdown cleanly: an eventfd in the epoll set wakes everything.
- Batch reads stay inside the source, because recvmmsg is CAN-socket specific and hidraw has no
  equivalent.
- **Virtual, not templated.** One virtual call per batch, not per frame; at batch 64 the vtable cost
  is below measurement noise. CRTP would force everything into headers for no measurable gain.
- Timestamping happens inside the source — earliest point in our own code.

### Record layout (40 bytes)

| Field | Type | Bytes | Note |
|---|---|---|---|
| `seq` | u64 | 8 | Global sequence, assigned at ring-claim time, not by the source |
| `t_kernel` | u64 | 8 | ns, **CLOCK_REALTIME** (SO_TIMESTAMPING is realtime-based) |
| `t_ingest` | u64 | 8 | ns, **CLOCK_MONOTONIC**, taken in the source |
| `can_id` | u32 | 4 | 29-bit extended included |
| `src_id` | u8 | 1 | Which bus — per-source FIFO guarantee and per-source counters |
| `len` | u8 | 1 | |
| `flags` | u8 | 1 | EFF / RTR / ERR |
| `_pad` | u8 | 1 | Explicit, never left to the compiler |
| `data` | u8[8] | 8 | Classic CAN only |

With the cell's 8-byte seqlock counter this is 48 bytes, padding to 64: **one cell per cache line.**

Two distinct things both called sequence — do not conflate:
- **Cell seqlock counter** = protocol. `pos*2 + phase`, odd/even carries write state. Never on the wire.
- **Record `seq` field** = data. `pos`. Goes on the wire; this is what produces gap markers.

CAN FD is out of scope. A 64-byte payload would push the record to 88 and break the cache line
alignment. Adding it later is the live demonstration of what the wire version byte is for.

### Clock strategy

- Per-frame timestamps stay **CLOCK_MONOTONIC** — local single-domain discipline preserved.
- The gateway publishes a **(monotonic, realtime) pair once per second** on the stats channel.
  Consumers convert as needed. Cost is 16 bytes per second, not per frame.
- This one mechanism solves both cross-domain problems: cross-machine latency (chrony disciplines
  CLOCK_REALTIME, never CLOCK_MONOTONIC) and the kernel-to-userspace delta (SO_TIMESTAMPING is
  realtime-based). Inter-sample drift is ppm, i.e. nanoseconds — negligible.
- **Cross-machine latency method: chrony.** Local path keeps genuine one-way CLOCK_MONOTONIC.
  Always state which method produced which number; never mix them in one plot.
- Mandatory for chrony to be defensible:
  - **Disable `makestep` during measurement runs.** A step mid-run silently corrupts the numbers.
    Slew only.
  - **Log `chronyc tracking` alongside every run** — offset and RMS jitter reported as an error bar.
  - Low-rate (1 Hz) consumer echo for an **RTT/2 cross-check**. Not a second method; a validator.
    If chrony and RTT/2 disagree by more than the stated error bar, the sync is bad and you know it.
- Known methodological weakness, state it: **the offered load degrades the clock sync it depends on.**
  Under a throughput test the USB-attached NIC saturates, chrony's own packets are delayed, and the
  offset estimate worsens exactly during measurement. Tolerable while remote latency is milliseconds
  and chrony error is ~100-200 us; not tolerable if remote latency approaches the error.
- vcan limitation: the kernel timestamp is taken when the frame enters the virtual stack, i.e. when
  the generator wrote it. This measures the loopback path, not a real controller RX path.

## Risks

1. **M2 is the schedule risk.** A correct MP broadcast ring can eat three weeks. M1's mutex queue is
   the insurance; never let M2 block M1 from being demoable.
2. **QEMU will not reproduce the ARM race.** Its memory model is stronger than real Cortex-A53.
   QEMU job for functional correctness; the memory-model demonstration runs on real hardware,
   results committed.
3. **vcan in containers.** Host kernel module, host netns. Gateway needs network_mode: host and
   NET_ADMIN; interfaces created on the host by a setup script.

## Known measurement traps

- `cangen` cannot load the pipeline and its pacing is sleep-bound — measures the scheduler, not the gateway.
  Own generator required; synthetic in-process source for the throughput ceiling.
- `SO_RXQ_OVFL` must be enabled or kernel-side drops go uncounted and the published drop rate is wrong.
- vcan is loopback: set CAN_RAW_RECV_OWN_MSGS / CAN_RAW_LOOPBACK deliberately.
- vcan has no bit timing, arbitration, error frames or bus-off. State as a limitation.
  Partial mitigation: frames with CAN_ERR_FLAG can be injected and received with an err_mask.
- vcan has no rate ceiling, so overload scenarios are synthetic and do not correspond to a 500 kbit/s bus.
- Kernel software RX timestamps are CLOCK_REALTIME. Take a CLOCK_MONOTONIC stamp at ingest for
  latency accounting. Never subtract across the two clock domains.
- Pi 3 B+ Ethernet is USB 2.0 attached, ~200-230 Mbit/s realistic, shared with the LIN/HID source.
  Establish the ceiling with iperf3 first and cite it.
- Pi 3 B+ thermal throttles at 80 C under sustained load. Report the knee rather than hiding it.
- Use the 64-bit OS (`uname -m` -> aarch64). armv7 complicates 64-bit atomic lock-freedom.
- Never measure performance under TSan (5-15x time, 5-10x memory). Separate CI jobs; TSan does not
  compose with ASan.

## Concurrency notes

- Memory ordering: `release` on slot publish, `acquire` on slot read, `relaxed` for statistics
  counters, `acq_rel` on a ticket RMW that also orders data. Justify every `seq_cst` or remove it.
- **Producer collision race:** with M producers doing fetch_add on a ring of size N, a stalled producer
  holding position P can collide with a producer that has claimed P+N. A seqlock assumes one writer and
  does not protect this. Producers must *claim* the cell (CAS its sequence word), not just store to it.
  Consequence: lock-free across cells, obstruction-free per cell. A reader skips a cell stuck odd and
  reports it as a gap — graceful degradation, and worth documenting.
- **TSan will not catch an insufficient memory order** on an atomic; it takes the declared order at face
  value. It catches missing synchronisation, not wrong synchronisation. The real-hardware aarch64 stress
  test is the necessary complement, not an optional extra.
- No allocation on the hot path. Fixed-size POD records in a preallocated ring. This dodges lock-free
  memory reclamation (hazard pointers / epochs) entirely.
- Design shutdown as a feature from the start: drain phase, and a blocking wrapper on the idle path so
  threads do not spin.
- Prove the test harness can fail: commit a deliberately-broken variant and show the stress test catches it.
