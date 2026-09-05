#!/usr/bin/env bash
# Runs the pipeline in repeated blocks for half an hour and records what the
# board's temperature does to the numbers.
#
# The Pi reports throttled=0x80000, soft-temp-limit-occurred, latched since
# boot. That flag says the firmware dropped the clock at some point because the
# die reached its soft limit, and every latency figure in results/ comes from a
# run short enough that it may never have happened during the measurement. A
# gateway on a shelf in a cabinet runs for weeks. So the question is not what
# this pipeline does for twelve seconds on a cool board, it is whether the
# numbers survive the board getting hot.
#
# Each block is a fresh measurement at a fixed offered rate. Temperature, core
# clock and the throttle bitmask are read immediately before it, so a row is a
# latency distribution next to the thermal state that produced it.
#
#   ./scripts/thermal.sh            # 30 blocks of 45s, about 25 minutes
#   BLOCKS=10 ./scripts/thermal.sh
set -uo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)" || exit 1

BIN="${MEASURE_BIN:-build/src}"
BLOCKS="${BLOCKS:-30}"
BLOCK_SECONDS="${BLOCK_SECONDS:-45}"
RATE="${RATE:-20000}"
PORT="${MEASURE_PORT:-9901}"
OUT="${THERMAL_OUT:-results/$(hostname)-$(date +%Y%m%d)/thermal}"

mkdir -p "$OUT"
./scripts/measure-env.sh capture > "$OUT/env.txt" 2>/dev/null
{
  echo "blocks=$BLOCKS"
  echo "block_seconds=$BLOCK_SECONDS"
  echo "rate=$RATE"
} >> "$OUT/env.txt"

read_temp() {
  if command -v vcgencmd >/dev/null 2>&1; then
    vcgencmd measure_temp 2>/dev/null | sed -n 's/temp=\([0-9.]*\).*/\1/p'
  else
    awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null
  fi
}

read_mhz() {
  if command -v vcgencmd >/dev/null 2>&1; then
    vcgencmd measure_clock arm 2>/dev/null | awk -F= '{printf "%.0f", $2/1000000}'
  else
    awk '{printf "%.0f", $1/1000}' \
      /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null
  fi
}

read_throttled() {
  if command -v vcgencmd >/dev/null 2>&1; then
    vcgencmd get_throttled 2>/dev/null | sed -n 's/throttled=//p'
  else
    echo "n/a"
  fi
}

us() { awk -v n="${1:-0}" 'BEGIN{printf "%.1f", n/1000}'; }

printf "%-7s %8s %7s %6s %10s %10s %10s %10s %8s\n" \
  block elapsed_s temp_c mhz p50 p90 p99 p99.9 throttled | tee "$OUT/summary.txt"

start=$(date +%s)
for ((b = 1; b <= BLOCKS; b++)); do
  run_dir="$OUT/block-$b"
  mkdir -p "$run_dir"

  # Read the thermal state before the block, not after: the block is what the
  # row describes, and reading afterwards would attribute a temperature the
  # measurement itself caused.
  temp=$(read_temp)
  mhz=$(read_mhz)
  thr=$(read_throttled)
  elapsed=$(( $(date +%s) - start ))

  "$BIN/etg-gateway" --source synth --rate "$RATE" --topology ring \
    --linger-us 100 --consumer probe:"$PORT" --seconds "$((BLOCK_SECONDS + 2))" \
    >/dev/null 2>"$run_dir/gateway.log" &
  gw=$!
  sleep 1
  "$BIN/etg-probe" --port "$PORT" --seconds "$BLOCK_SECONDS" --warmup 3 \
    >/dev/null 2>"$run_dir/probe.log"
  wait "$gw" 2>/dev/null

  lat=$(grep -A1 '^gateway ' "$run_dir/probe.log" | tail -1)
  p50=$(echo "$lat" | sed -n 's/.*p50 \([0-9-]*\).*/\1/p')
  p90=$(echo "$lat" | sed -n 's/.*p90 \([0-9-]*\).*/\1/p')
  p99=$(echo "$lat" | sed -n 's/.*p99 \([0-9-]*\).*/\1/p')
  p999=$(echo "$lat" | sed -n 's/.*p99\.9 \([0-9-]*\).*/\1/p')

  printf "%-7s %8s %7s %6s %10s %10s %10s %10s %8s\n" \
    "$b" "$elapsed" "${temp:-?}" "${mhz:-?}" \
    "$(us "${p50:-0}")" "$(us "${p90:-0}")" "$(us "${p99:-0}")" "$(us "${p999:-0}")" \
    "${thr:-?}" | tee -a "$OUT/summary.txt"
done

{
  echo
  echo "Microseconds. Each row is one $BLOCK_SECONDS s measurement at $RATE"
  echo "frames/s, with the die temperature, ARM clock and throttle bitmask read"
  echo "immediately before it."
  echo
  echo "The throttle word is a bitmask, and its upper bits are latched since"
  echo "boot rather than current: 0x80000 means the soft temperature limit was"
  echo "reached at some point, 0x8 means it is being throttled right now."
  echo "scripts/measure-env.sh decodes it."
} | tee -a "$OUT/summary.txt"
