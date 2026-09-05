#!/usr/bin/env bash
# Brings the compose stack up against a real vcan interface, drives it with the
# generator, and checks that the recorder's capture parses.
#
# This is the seam the native smoke test cannot cover: image build, container
# boundaries, host-namespace CAN access, and consumers that connect over TCP
# from separate containers.
#
# The image is built from debian:bookworm, which ships GCC 12 - the same
# compiler as Raspberry Pi OS bookworm. A build failure here is a warning that
# the code would not compile on the measurement target.
set -euo pipefail

SECONDS_RUN="${SECONDS_RUN:-5}"
RATE="${RATE:-2000}"
IFACE="${IFACE:-vcan0}"

cd "$(dirname "$0")/.."

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE does not exist; run scripts/setup-vcan.sh first" >&2
  exit 1
fi

export ETG_SOURCE="can:${IFACE}"
export ETG_IFACE="$IFACE"
export ETG_RATE="$RATE"
export ETG_SECONDS="$SECONDS_RUN"

cleanup() {
  echo
  echo "--- tearing down ---"
  docker compose --profile vcan down --volumes --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "--- building ---"
docker compose build --quiet

echo "--- starting gateway and consumers ---"
docker compose up -d gateway probe recorder

# Consumers use --reconnect, so they tolerate coming up before the gateway is
# listening. Give the stack a moment to settle before generating.
sleep 3

echo "--- generating ${RATE}/s for ${SECONDS_RUN}s on ${IFACE} ---"
docker compose --profile vcan run --rm generator

sleep 2

echo
echo "--- gateway ---"
docker compose logs --no-log-prefix --tail 6 gateway

echo
echo "--- probe ---"
docker compose logs --no-log-prefix --tail 4 probe

echo
echo "--- capture, parsed from the spec inside the container ---"
# Parsed in a container that mounts the same volume, so the check runs against
# the bytes the recorder actually wrote rather than a copy.
parsed=$(docker compose run --rm --entrypoint "" -v etg-data:/data recorder \
  python3 /usr/local/bin/parse-capture.py /data/capture.etg)
echo "$parsed"

frames=$(echo "$parsed" | sed -n 's/^frames  *\([0-9][0-9]*\).*/\1/p')
expected=$((RATE * SECONDS_RUN))
floor=$((expected * 80 / 100))

echo
echo "frames=${frames} floor=${floor}"

if [ -z "$frames" ] || [ "$frames" -lt "$floor" ]; then
  echo "FAIL: capture holds ${frames:-0} frames, expected at least ${floor}" >&2
  exit 1
fi

echo "OK"
