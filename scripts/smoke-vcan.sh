#!/usr/bin/env bash
# End-to-end smoke test over a real vcan interface: the generator writes frames,
# the gateway tap reads them back through PF_CAN, and the frame count is checked
# against what was asked for.
#
# This is the first test that exercises the generator at all, because the
# generator needs a CAN interface and the default WSL2 kernel cannot provide
# one. It runs in CI and on the Pi.
set -euo pipefail

BUILD="${1:-build}"
RATE="${RATE:-2000}"
SECONDS_RUN="${SECONDS_RUN:-3}"
IFACE="${IFACE:-vcan0}"

GATEWAY="${BUILD}/src/etg-gateway"
GEN="${BUILD}/src/etg-gen"

for bin in "$GATEWAY" "$GEN"; do
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
# The tap outlives the generator so it is already listening before the first
# frame and still listening after the last.
tap_seconds=$((SECONDS_RUN + 3))

echo "expecting ~${expected} frames at ${RATE}/s over ${SECONDS_RUN}s on ${IFACE}"

tap_out=$(mktemp)
"$GATEWAY" --source "can:${IFACE}" --seconds "$tap_seconds" --quiet >/dev/null 2>"$tap_out" &
tap_pid=$!

# Give the tap time to bind before generating.
sleep 1

"$GEN" --interface "$IFACE" --rate "$RATE" --seconds "$SECONDS_RUN"

wait "$tap_pid"

echo
echo "--- tap ---"
cat "$tap_out"

frames=$(sed -n 's/.*frames=\([0-9]*\).*/\1/p' "$tap_out")
drops=$(sed -n 's/.*kernel_drops=\([0-9]*\).*/\1/p' "$tap_out")
rm -f "$tap_out"

if [ -z "$frames" ]; then
  echo "could not parse a frame count from the tap output" >&2
  exit 1
fi

# A loose lower bound on purpose. This asserts the path works end to end; it is
# not a performance measurement, and treating it as one would put a flaky
# threshold in CI. Real numbers come from M4 on the Pi.
floor=$((expected * 90 / 100))
echo
echo "frames=${frames} kernel_drops=${drops} floor=${floor}"

if [ "$frames" -lt "$floor" ]; then
  echo "FAIL: tap saw ${frames} frames, expected at least ${floor}" >&2
  exit 1
fi

echo "OK"
