#!/usr/bin/env bash
# Brings the compose stack up against a real vcan interface, drives it with the
# generator, and checks that every consumer actually consumed.
#
# This is the seam the native smoke test cannot cover: image build, container
# boundaries, host-namespace CAN access, and consumers connecting over TCP from
# separate containers.
#
# It uses the vcan override, so it needs a Linux host - see
# docker-compose.vcan.yml for why that is a kernel constraint rather than a
# preference.
#
# The image is built from debian:bookworm, which ships GCC 12 - the same
# compiler as Raspberry Pi OS bookworm. A build failure here is a warning that
# the code would not compile on the measurement target.
set -euo pipefail

SECONDS_RUN="${SECONDS_RUN:-5}"
RATE="${RATE:-2000}"
IFACE="${IFACE:-vcan0}"
HTTP_PORT="${ETG_HTTP_PORT:-8080}"

cd "$(dirname "$0")/.."

# Both files, everywhere. The override is what puts the services into the
# namespace where vcan0 exists.
compose() {
  docker compose -f docker-compose.yml -f docker-compose.vcan.yml "$@"
}

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE does not exist; run scripts/setup-vcan.sh first" >&2
  exit 1
fi

export ETG_SOURCE="can:${IFACE}"
export ETG_IFACE="$IFACE"
export ETG_RATE="$RATE"
export ETG_SECONDS="$SECONDS_RUN"
export ETG_HTTP_PORT="$HTTP_PORT"

expected=$((RATE * SECONDS_RUN))
floor=$((expected * 80 / 100))

cleanup() {
  echo
  echo "--- tearing down ---"
  compose --profile vcan down --volumes --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "--- building ---"
compose build --quiet

echo "--- starting gateway and consumers ---"
compose up -d gateway probe recorder view

# Consumers reconnect on their own, so they tolerate coming up before the
# gateway is listening. Give the stack a moment to settle before generating.
sleep 3

echo "--- generating ${RATE}/s for ${SECONDS_RUN}s on ${IFACE} ---"
compose --profile vcan run --rm generator

sleep 2

echo
echo "--- gateway ---"
compose logs --no-log-prefix --tail 6 gateway

echo
echo "--- probe ---"
compose logs --no-log-prefix --tail 4 probe

# Checking the view means asking it what it saw, not just whether its container
# is running. It is a consumer like the others and is held to the same floor.
echo
echo "--- live view ---"
view_json=$(compose exec -T view python3 -c "
import json, urllib.request
d = json.loads(urllib.request.urlopen('http://127.0.0.1:${HTTP_PORT}/stats.json').read())
print(d['connected'], d['frames'], d['markers'], d['silent'], len(d['ids']))")
echo "connected/frames/markers/silent/ids: ${view_json}"

view_frames=$(echo "$view_json" | awk '{print $2}')
if [ -z "$view_frames" ] || [ "$view_frames" -lt "$floor" ]; then
  echo "FAIL: view saw ${view_frames:-0} frames, expected at least ${floor}" >&2
  exit 1
fi

# Stop the recorder before reading its file. It buffers, so a capture read from
# under a running writer ends mid-message - which is legitimate, but it means
# the file cannot be checked for completeness. SIGTERM makes it flush and close.
echo
echo "--- stopping the recorder so its capture is complete ---"
compose stop recorder

echo
echo "--- capture, parsed from the spec inside the container ---"
# Parsed in a container that mounts the same volume, so the check runs against
# the bytes the recorder actually wrote rather than a copy.
parsed=$(compose run --rm --entrypoint "" -v etg-data:/data recorder \
  python3 /usr/local/bin/parse-capture.py --strict /data/capture.etg)
echo "$parsed"

frames=$(echo "$parsed" | sed -n 's/^frames  *\([0-9][0-9]*\).*/\1/p')

echo
echo "recorded=${frames} view=${view_frames} floor=${floor}"

if [ -z "$frames" ] || [ "$frames" -lt "$floor" ]; then
  echo "FAIL: capture holds ${frames:-0} frames, expected at least ${floor}" >&2
  exit 1
fi

echo "OK"
