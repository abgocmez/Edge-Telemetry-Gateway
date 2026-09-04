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

- **M1 — visibly runs.** vcan0, own paced load generator (not cangen), one source thread,
  **plain mutex + condvar bounded queue**, TCP egress, probe printing frames/sec.
  CMake + Ninja, docker compose up, CI build + smoke test.
  The mutex queue is deliberate: it de-risks M2 and yields a free mutex-vs-lockfree comparison.
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
