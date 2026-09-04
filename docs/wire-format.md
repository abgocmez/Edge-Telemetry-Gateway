# Wire format — version 1

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
| 4 | 1 | `version` | `1` |
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
| 3-7 | reserved | must be `0`; a decoder rejects a record that sets one |

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
- **At-most-once delivery.** A gap in `seq` is real loss, detectable by the
  consumer. Explicit gap records are added in M3.

## Not in version 1

- CAN FD. A 64-byte payload would make the record 88 bytes and break the
  one-cell-per-cache-line property. Adding it is a version bump, and is the
  worked example of what `version` and `record_len` are for.
- Explicit gap records (M3).
- Compression, encryption, authentication.
