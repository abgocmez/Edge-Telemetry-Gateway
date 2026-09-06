# Wire format — version 3

Normative. The implementation in `src/wire.cpp` follows this document, and
`tests/test_wire.cpp` holds a hand-written golden byte vector that fails if
either drifts from the other.

## Rules

- **Little-endian**, always, on every field. Both targets (x86_64 and
  Cortex-A53) are little-endian, so the byte-order code costs nothing at
  runtime — but the order is specified here, not inherited from the host.
- **Fixed layout.** No padding, no alignment requirements, no optional fields.
- **Parsed field by field.** A receive buffer is never cast to a struct, even
  though the in-memory `Frame` currently has the same layout. That coincidence
  is not load-bearing. Casting would make this a shared C++ header rather than
  a protocol, and would make it untrue that another language could implement it.
- **Reserved bits must be zero.** A decoder rejects a record that sets one.
  Adding a field later is therefore a version bump, never a silent misread.

## Message

```
+------------------+
|  BatchHeader     |  16 bytes
+------------------+
|  FrameRecord[0]  |  40 bytes
|  ...             |
|  FrameRecord[n-1]|
+------------------+
```

A reader consumes 16 bytes, validates the header, then reads exactly
`payload_len` more bytes. The header is the message delimiter; there is no
separate length prefix.

## BatchHeader (16 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `magic` | `45 54 47 31` — ASCII `ETG1` |
| 4 | 1 | `version` | `3` |
| 5 | 1 | `header_len` | `16` |
| 6 | 2 | `record_len` | `40` |
| 8 | 4 | `count` | number of records that follow |
| 12 | 4 | `payload_len` | `count * record_len` |

`header_len` and `record_len` are on the wire so a future reader can skip a
batch it does not understand rather than misparse it. `payload_len` is
redundant with `count * record_len` and is validated against it — a
disagreement is rejected before any record is read.

## FrameRecord (40 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `seq` | Global sequence, assigned at ring-claim time. Monotonic per gateway. Gaps are real loss. |
| 8 | 8 | `t_kernel_ns` | **CLOCK_REALTIME**, from `SO_TIMESTAMPING` |
| 16 | 8 | `t_ingest_ns` | **CLOCK_MONOTONIC**, taken inside the source |
| 24 | 4 | `can_id` | 11-bit, or 29-bit when `EXTENDED` is set |
| 28 | 1 | `src_id` | Which bus. Per-source FIFO is guaranteed within one `src_id` |
| 29 | 1 | `len` | `0..8`. A decoder rejects `> 8` |
| 30 | 1 | `flags` | see below |
| 31 | 1 | `reserved` | must be `0` |
| 32 | 8 | `data` | payload; bytes beyond `len` are unspecified |

### flags

| Bit | Name | Meaning |
|---|---|---|
| 0 | `EXTENDED` | 29-bit identifier |
| 1 | `REMOTE` | RTR frame |
| 2 | `ERROR` | `CAN_ERR_FLAG` was set on the frame |
| 3 | `GAP` | **v2.** Not a frame: a loss marker. See below |
| 4 | `ECHO` | **v3.** Not a frame: a round-trip probe. See below |
| 5-7 | reserved | must be `0`; a decoder rejects a record that sets one |

## The two timestamps are in different clock domains

This is deliberate and must not be papered over by subtracting one from the
other.

- `t_ingest_ns` is `CLOCK_MONOTONIC`, so local latency measurement never sees a
  clock step.
- `t_kernel_ns` is `CLOCK_REALTIME`, because that is what `SO_TIMESTAMPING`
  provides. It is also the domain `chrony` disciplines, which is what makes
  cross-machine comparison possible at all.

To convert between them, use the `(monotonic, realtime)` pair the gateway
publishes once per second on the stats channel. Inter-sample drift is in the
parts-per-million range, i.e. nanoseconds. Never subtract a value in one domain
from a value in the other directly.

## Guarantees this format carries

- **Per-source FIFO.** Records with the same `src_id` arrive in the order that
  source produced them.
- **No total order across sources.** The interleaving of different `src_id`
  values reflects ring ticket-claim order, not bus arrival time.
- **At-most-once delivery.** Loss is reported explicitly by a gap marker, not
  left to be inferred from a jump in `seq`.

## Gap markers (version 2)

A record with `GAP` set is not a frame. It reports loss:

| Field | Meaning on a marker |
|---|---|
| `seq` | the first sequence this consumer did **not** receive |
| `data` | u64, little-endian: how many frames are missing |
| `src_id` | the reason, **not** a bus number. See the table below |
| `t_ingest_ns` | when the gateway emitted the marker |
| `t_kernel_ns` | `0`; a marker was never on a bus |
| `can_id`, `len`, `flags` | `0`, `8`, `GAP` |

The next real frame therefore has sequence `seq + count`, so a consumer can
close its books without guessing where the stream resumes.

### Reasons

| `src_id` | Reason | A fault? |
|---|---|---|
| 0 | `consumer_overrun` — this consumer fell behind and was overwritten | yes |
| 1 | `ingest_loss` — the kernel dropped them; nobody ever had them | yes |
| 2 | `consumer_absent` — this consumer was disconnected while these went past | **no** |

The third is a discontinuity rather than a failure, and conflating it with the
other two would make every restart look like a fault and inflate every drop
figure that is ever quoted. It is still reported, because a recorder appending
to a file must not leave an unexplained jump in it: a capture that silently
skips is a recording that lies about what it contains, and no reader can
distinguish that from corruption.

A consumer that reconnects fast enough receives no marker at all — its frames
were simply waiting in the buffer, and there is no hole to describe. The marker
appears only once the outage outlasts the buffer.

**Markers are emitted by the gateway, not inferred by the consumer**, because
only the gateway can say *why*. A consumer can see that sequences 100 to 149
never arrived; it cannot tell whether it was too slow or whether the kernel
dropped them upstream of the pipeline — and those two facts call for entirely
different responses. The second case does not even produce a jump in `seq`:
frames the kernel discarded never received a sequence number, so the stream
stays dense and the loss is invisible without a marker.

A consumer should still track sequence jumps that arrive with **no** preceding
marker. Under version 2 that must not happen, so it is not a fallback detector —
it is the check that the marker mechanism is working.

## Echo probes (version 3)

A record with `ECHO` set is not a frame either. The gateway emits one
periodically; a consumer **sends it straight back, unchanged, on the same
connection** and does not count it, timestamp it, or record it.

| Field | Meaning on an echo |
|---|---|
| `seq` | a nonce, **not** a sequence |
| `t_ingest_ns` | when the gateway sent it, on the gateway's own clock |
| everything else | zero |

This is the reverse direction of the connection, and the only use of it.

### Why an echo rather than a one-way timestamp

Because one-way latency between two machines requires their clocks to agree, and
measuring it on the pair this was built for showed that they do not: the
consumer's host had never completed a single NTP exchange, and the two
`CLOCK_REALTIME` clocks differed by 1.19 seconds. An echo is timed by one clock,
on one machine, from send to return — there is nothing to agree about.

It travels **in-band**, through the same batching and the same socket as the
frames around it, so what it measures is what a frame queued behind real traffic
experiences rather than what an idle socket would report. Measured across a LAN
at 20 000 frames/s against an independent side-channel doing the same thing over
the same link:

| | idle channel | in-band |
|---|---|---|
| RTT p50 | 518 µs | 496.1 µs |
| RTT p99 | 2 294 µs | 655.7 µs |

The medians agree to within 4%, which is the cross-check: two mechanisms sharing
no code arrive at the same figure, so neither is measuring something else. The
tails are not comparable — the side-channel's responder is a Python script and
the in-band one is the C++ consumer, so they describe two responders rather than
two paths.

Three caveats stated rather than corrected for: halving assumes the path is
symmetric, the consumer's turnaround is inside the figure, and a probe answered
on the consumer's read loop measures that loop's health as well as the link's.

## Why version 2 exists

Version 1 declared bits 3-7 reserved and required a decoder to reject any record
that set one. Gap markers need a bit, so introducing them was necessarily a
version bump.

That refusal is the feature, not an obstacle to work around. Had the bit been
quietly reused at version 1, every already-deployed consumer would have begun
reading loss markers as CAN frames with identifier `0` and a payload that is
really a count. Instead they refuse the stream and say why. This is what the
version byte was reserved for; it turned out to be gap markers rather than CAN
FD, which was the original guess.

## Not in version 3

- CAN FD. A 64-byte payload would make the record 88 bytes and break the
  one-cell-per-cache-line property. Another version bump when it arrives.
- Compression, encryption, authentication.
