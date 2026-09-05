#!/usr/bin/env bash
# Full path smoke test:
#
#   etg-gen --> vcan0 --> CanSource --> fan-out --> queue --> egress --> TCP --> etg-probe
#
# Every component in M1 is on this path, which is the point: unit tests cover
# the pieces, this covers the seams between them.
#
# It runs in CI and on the Pi. It cannot run on WSL2, whose kernel has
# CONFIG_CAN and CONFIG_CAN_RAW but not CONFIG_CAN_VCAN.
set -euo pipefail

BUILD="${1:-build}"
RATE="${RATE:-2000}"
SECONDS_RUN="${SECONDS_RUN:-3}"
IFACE="${IFACE:-vcan0}"
PORT="${PORT:-9101}"

GATEWAY="${BUILD}/src/etg-gateway"
GEN="${BUILD}/src/etg-gen"
PROBE="${BUILD}/src/etg-probe"

for bin in "$GATEWAY" "$GEN" "$PROBE"; do
  if [ ! -x "$bin" ]; then
    echo "missing binary: $bin" >&2
    exit 1
  fi
done

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE does not exist; run scripts/setup-vcan.sh first" >&2
  exit 1
fi

expected=$((RATE * SECONDS_RUN))
run_seconds=$((SECONDS_RUN + 4))

echo "expecting ~${expected} frames at ${RATE}/s over ${SECONDS_RUN}s on ${IFACE} via port ${PORT}"

gw_log=$(mktemp)
probe_log=$(mktemp)
cleanup() { rm -f "$gw_log" "$probe_log"; }
trap cleanup EXIT

"$GATEWAY" --source "can:${IFACE}" --consumer "probe:${PORT}" --seconds "$run_seconds" \
  >/dev/null 2>"$gw_log" &
gw_pid=$!
sleep 1

"$PROBE" --port "$PORT" --seconds "$((run_seconds - 1))" >/dev/null 2>"$probe_log" &
probe_pid=$!
sleep 1

"$GEN" --interface "$IFACE" --rate "$RATE" --seconds "$SECONDS_RUN"

wait "$probe_pid"
wait "$gw_pid"

echo
echo "--- gateway ---"
tail -5 "$gw_log"
echo
echo "--- probe ---"
tail -8 "$probe_log"

received=$(sed -n 's/^frames  *\([0-9]*\) .*/\1/p' "$probe_log")
proto_errs=$(sed -n 's/^protocol_errs  *\([0-9]*\).*/\1/p' "$probe_log")
missing=$(sed -n 's/^missing  *\([0-9]*\).*/\1/p' "$probe_log")

if [ -z "$received" ] || [ -z "$proto_errs" ]; then
  echo "could not parse the probe output" >&2
  exit 1
fi

echo
echo "received=${received} missing=${missing} protocol_errs=${proto_errs}"

# A protocol error is never acceptable: it means the two ends disagree about the
# wire format, which no amount of load should be able to cause.
if [ "$proto_errs" -ne 0 ]; then
  echo "FAIL: ${proto_errs} protocol errors" >&2
  exit 1
fi

# A loose lower bound on purpose. This asserts the seams hold; it is not a
# performance measurement, and treating it as one would put a flaky threshold in
# CI. Real numbers come from M4 on the Pi.
floor=$((expected * 90 / 100))
if [ "$received" -lt "$floor" ]; then
  echo "FAIL: probe received ${received} frames, expected at least ${floor}" >&2
  exit 1
fi

echo "OK"
