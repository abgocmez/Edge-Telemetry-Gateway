#!/usr/bin/env python3
"""Parse an etg capture file using only docs/wire-format.md.

This exists to keep one specific claim honest. Both ends of the protocol are
written in C++, so nothing stops them sharing a header and calling that a
protocol. This parser was written from the specification table alone and shares
no code, no header and no language with the implementation. If the format were
really just a struct layout, this would not work.

It is also the check that the recorder's output is genuinely self-describing:
the file is a stream of complete wire messages, so a capture can be replayed by
piping it into a consumer with no separate reader.

    scripts/parse-capture.py capture.etg
    scripts/parse-capture.py capture.etg --json | head
"""

import argparse
import json
import struct
import sys

MAGIC = b"ETG1"
VERSION = 1
HEADER_SIZE = 16
RECORD_SIZE = 40

FLAG_EXTENDED = 1 << 0
FLAG_REMOTE = 1 << 1
FLAG_ERROR = 1 << 2
FLAG_RESERVED_MASK = 0xF8

# Little-endian, per the spec. Never native order: the point is that the byte
# order is defined by the document, not by whatever CPU happens to run this.
HEADER = struct.Struct("<4sBBHII")   # magic, version, header_len, record_len, count, payload_len
RECORD = struct.Struct("<QQQIBBBB8s")  # seq, t_kernel, t_ingest, can_id, src, len, flags, pad, data


class ProtocolError(Exception):
    pass


class TruncatedTail(Exception):
    """The file ends part-way through a message.

    Not the same thing as a malformed file. A capture read while the recorder is
    still appending legitimately ends mid-message, because the writer's buffer
    has not been flushed yet. Treating that as corruption would make any live
    read a false alarm, so it is reported separately and --strict decides
    whether it is fatal.
    """

    def __init__(self, nbytes):
        super().__init__(f"{nbytes} trailing bytes")
        self.nbytes = nbytes


def parse_header(buf):
    magic, version, header_len, record_len, count, payload_len = HEADER.unpack(buf)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic {magic!r}")
    if version != VERSION:
        raise ProtocolError(f"unsupported version {version}")
    if header_len != HEADER_SIZE:
        raise ProtocolError(f"bad header length {header_len}")
    if record_len != RECORD_SIZE:
        raise ProtocolError(f"bad record length {record_len}")
    if payload_len != count * record_len:
        raise ProtocolError(f"payload {payload_len} disagrees with {count} x {record_len}")
    return count, payload_len


def parse_record(buf):
    seq, t_kernel, t_ingest, can_id, src_id, length, flags, pad, data = RECORD.unpack(buf)
    if flags & FLAG_RESERVED_MASK:
        raise ProtocolError(f"reserved flag bit set in {flags:#04x}")
    if length > 8:
        raise ProtocolError(f"length {length} exceeds classic CAN")
    if pad != 0:
        raise ProtocolError("reserved byte is not zero")
    return {
        "seq": seq,
        "t_kernel_ns": t_kernel,
        "t_ingest_ns": t_ingest,
        "can_id": can_id,
        "src_id": src_id,
        "len": length,
        "extended": bool(flags & FLAG_EXTENDED),
        "remote": bool(flags & FLAG_REMOTE),
        "error": bool(flags & FLAG_ERROR),
        "data": data[:length].hex(),
    }


def read_capture(path):
    with open(path, "rb") as f:
        while True:
            head = f.read(HEADER_SIZE)
            if not head:
                return
            if len(head) != HEADER_SIZE:
                raise TruncatedTail(len(head))
            count, payload_len = parse_header(head)
            payload = f.read(payload_len)
            if len(payload) != payload_len:
                raise TruncatedTail(HEADER_SIZE + len(payload))
            for i in range(count):
                yield parse_record(payload[i * RECORD_SIZE:(i + 1) * RECORD_SIZE])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--json", action="store_true", help="one JSON object per frame")
    ap.add_argument("--limit", type=int, default=0, help="stop after N frames")
    ap.add_argument("--strict", action="store_true",
                    help="fail on a truncated tail; use when the writer has stopped")
    args = ap.parse_args()

    frames = 0
    batches_seen = set()
    gaps = 0
    missing = 0
    last_seq = None
    per_source = {}
    tail = 0

    try:
        for rec in read_capture(args.path):
            if args.json:
                print(json.dumps(rec))
            frames += 1
            per_source[rec["src_id"]] = per_source.get(rec["src_id"], 0) + 1
            batches_seen.add(rec["can_id"])
            if last_seq is not None and rec["seq"] != last_seq + 1:
                gaps += 1
                if rec["seq"] > last_seq:
                    missing += rec["seq"] - last_seq - 1
            last_seq = rec["seq"]
            if args.limit and frames >= args.limit:
                break
    except TruncatedTail as exc:
        tail = exc.nbytes
    except ProtocolError as exc:
        print(f"protocol error after {frames} frames: {exc}", file=sys.stderr)
        return 1

    if not args.json:
        print(f"frames      {frames}")
        print(f"can_ids     {len(batches_seen)}")
        print(f"sources     {dict(sorted(per_source.items()))}")
        print(f"gaps        {gaps}")
        print(f"missing     {missing}")
        if tail:
            print(f"tail        {tail} trailing bytes (writer still appending?)")

    if tail and args.strict:
        print(f"truncated tail of {tail} bytes with --strict", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
