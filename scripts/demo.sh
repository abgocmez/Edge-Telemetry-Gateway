#!/usr/bin/env bash
# Runs the whole pipeline against a real vcan interface and leaves it running,
# so the live view can be opened from another machine on the network.
#
#   ./scripts/demo.sh start [rate]
#   ./scripts/demo.sh status
#   ./scripts/demo.sh stop
#
# Everything runs natively rather than in containers, because this is meant for
# the measurement target: it is the same set of processes the compose stack
# starts, without the image build in the way.
#
# Nothing here runs forever. The generator is bounded, and stop tears the whole
# set down including the vcan interface it created.
set -uo pipefail

RATE="${2:-2000}"
IDS="${DEMO_IDS:-8}"
DURATION="${DEMO_SECONDS:-3600}"
IFACE="${DEMO_IFACE:-vcan0}"
HTTP_PORT="${DEMO_HTTP_PORT:-8080}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${DEMO_BIN:-$ROOT/build/src}"
RUN="${DEMO_RUN_DIR:-$HOME/.etg-demo}"

mkdir -p "$RUN"

pidfile() { echo "$RUN/$1.pid"; }
logfile() { echo "$RUN/$1.log"; }

running() {
  local pf
  pf=$(pidfile "$1")
  [ -f "$pf" ] && kill -0 "$(cat "$pf")" 2>/dev/null
}

spawn() {
  local name="$1"; shift
  if running "$name"; then
    echo "  $name already running (pid $(cat "$(pidfile "$name")"))"
    return
  fi
  nohup "$@" >"$(logfile "$name")" 2>&1 &
  echo $! > "$(pidfile "$name")"
  echo "  $name started (pid $!)"
}

kill_one() {
  local name="$1" pf
  pf=$(pidfile "$name")
  if [ -f "$pf" ]; then
    local pid
    pid=$(cat "$pf")
    if kill -0 "$pid" 2>/dev/null; then
      # SIGTERM, not SIGKILL: every component here handles it and flushes. The
      # recorder in particular must close its file, or the capture ends
      # mid-record and cannot be checked for completeness.
      kill -TERM "$pid" 2>/dev/null
      for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
      done
      kill -KILL "$pid" 2>/dev/null
      echo "  $name stopped"
    fi
    rm -f "$pf"
  fi
}

start() {
  for b in etg-gateway etg-gen etg-view etg-record etg-probe; do
    if [ ! -x "$BIN/$b" ]; then
      echo "missing binary: $BIN/$b" >&2
      echo "build first: cmake -S . -B build -G Ninja -DETG_BUILD_TESTS=OFF && cmake --build build" >&2
      exit 1
    fi
  done

  if ! ip link show "$IFACE" >/dev/null 2>&1; then
    echo "bringing up $IFACE"
    sudo modprobe vcan || exit 1
    sudo ip link add dev "$IFACE" type vcan || exit 1
    sudo ip link set up "$IFACE" || exit 1
  fi

  echo "starting:"
  # Order does not matter - every consumer reconnects - but the gateway first
  # keeps the logs readable.
  spawn gateway "$BIN/etg-gateway" --source "can:$IFACE" --rate "$RATE" \
    --consumer probe:9001 --consumer recorder:9002 --consumer view:9003 --seconds 0
  sleep 1
  spawn view     "$BIN/etg-view"   --host 127.0.0.1 --port 9003 --http "$HTTP_PORT"
  spawn recorder "$BIN/etg-record" --host 127.0.0.1 --port 9002 --reconnect \
    --out "$RUN/capture.etg"
  spawn probe    "$BIN/etg-probe"  --host 127.0.0.1 --port 9001 --reconnect --warmup 2
  sleep 1
  spawn gen      "$BIN/etg-gen" --interface "$IFACE" --rate "$RATE" \
    --id 100 --ids "$IDS" --seconds "$DURATION"

  local ip
  ip=$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7; exit}')
  echo
  echo "  live view:  http://${ip:-<this-host>}:${HTTP_PORT}/"
  echo "  generating: ${RATE}/s across ${IDS} ids on ${IFACE} for ${DURATION}s"
  echo "  logs:       $RUN"
}

status() {
  for n in gateway view recorder probe gen; do
    if running "$n"; then
      printf '  %-9s running (pid %s)\n' "$n" "$(cat "$(pidfile "$n")")"
    else
      printf '  %-9s stopped\n' "$n"
    fi
  done
  if [ -f "$RUN/capture.etg" ]; then
    printf '  %-9s %s\n' capture "$(du -h "$RUN/capture.etg" | cut -f1)"
  fi
}

stop() {
  echo "stopping:"
  # Generator first, so the pipeline drains before the consumers go away and the
  # final counters mean something.
  for n in gen probe recorder view gateway; do
    kill_one "$n"
  done
  if ip link show "$IFACE" >/dev/null 2>&1; then
    sudo ip link del "$IFACE" 2>/dev/null && echo "  $IFACE removed"
  fi
}

case "${1:-status}" in
  start)  start ;;
  stop)   stop ;;
  status) status ;;
  *) echo "usage: $0 [start [rate]|stop|status]" >&2; exit 2 ;;
esac
