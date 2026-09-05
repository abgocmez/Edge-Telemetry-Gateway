#!/usr/bin/env bash
# Runs a named experiment and writes its results, raw output and environment to
# results/<host>-<date>/<experiment>/.
#
#   ./scripts/measure.sh latency
#   ./scripts/measure.sh linger
#   ./scripts/measure.sh overload
#   ./scripts/measure.sh topology
#   ./scripts/measure.sh all
#
# Every run captures scripts/measure-env.sh output alongside its numbers, because
# a latency figure without its governor, die temperature, isolated core and clock
# discipline is not a result: nobody can reproduce it or argue with it. The same
# binary on the same board reports a different tail depending on all four.
#
# Uses the synthetic source, so no vcan and no CAN hardware are needed and the
# offered rate is a knob rather than a property of a bus. The vcan path is
# covered by scripts/smoke-vcan.sh; what is measured here is the pipeline, and
# mixing in a loopback driver would only add a variable nobody asked about.
set -uo pipefail

cd "$(cd "$(dirname "$0")/.." && pwd)"

BIN="${MEASURE_BIN:-build/src}"
SECONDS_RUN="${MEASURE_SECONDS:-12}"
WARMUP="${MEASURE_WARMUP:-3}"
PORT="${MEASURE_PORT:-9900}"
OUT_ROOT="${MEASURE_OUT:-results/$(hostname)-$(date +%Y%m%d)}"

for b in etg-gateway etg-probe; do
  if [ ! -x "$BIN/$b" ]; then
    echo "missing binary: $BIN/$b" >&2
    echo "build first: cmake -S . -B build -G Ninja -DETG_BUILD_TESTS=OFF && cmake --build build" >&2
    exit 1
  fi
done

# ---------------------------------------------------------------- helpers ---

# One run. Prints "frames rate p50 p90 p99 p999 max batch lost" on stdout and
# leaves the full logs in $run_dir.
run_once() {
  local run_dir="$1" topology="$2" rate="$3" batch="$4" linger="$5" capacity="$6" stall="$7"
  mkdir -p "$run_dir"

  "$BIN/etg-gateway" --topology "$topology" --source synth --rate "$rate" \
    --capacity "$capacity" --batch "$batch" --linger-us "$linger" \
    --consumer probe:"$PORT" --seconds "$((SECONDS_RUN + 2))" \
    >/dev/null 2>"$run_dir/gateway.log" &
  local gw=$!
  sleep 1

  "$BIN/etg-probe" --port "$PORT" --seconds "$SECONDS_RUN" --warmup "$WARMUP" \
    ${stall:+--stall-us "$stall"} >/dev/null 2>"$run_dir/probe.log"
  wait "$gw" 2>/dev/null

  local lat frames rate_out batch_mean lost
  lat=$(grep -A1 '^gateway ' "$run_dir/probe.log" | tail -1)
  frames=$(sed -n 's/^frames  *\([0-9][0-9]*\) .*/\1/p' "$run_dir/probe.log")
  rate_out=$(sed -n 's/.*= \([0-9.]*\)\/s/\1/p' "$run_dir/probe.log" | head -1)
  batch_mean=$(sed -n 's/.*mean \([0-9.]*\) frames.*/\1/p' "$run_dir/probe.log")
  lost=$(sed -n 's/^markers  *[0-9][0-9]*  *(\([0-9][0-9]*\) frames.*/\1/p' "$run_dir/probe.log")

  local p50 p90 p99 p999 pmax
  p50=$(echo "$lat" | sed -n 's/.*p50 \([0-9-]*\).*/\1/p')
  p90=$(echo "$lat" | sed -n 's/.*p90 \([0-9-]*\).*/\1/p')
  p99=$(echo "$lat" | sed -n 's/.*p99 \([0-9-]*\).*/\1/p')
  p999=$(echo "$lat" | sed -n 's/.*p99\.9 \([0-9-]*\).*/\1/p')
  pmax=$(echo "$lat" | sed -n 's/.*max \([0-9-]*\).*/\1/p')

  echo "${frames:-0} ${rate_out:-0} ${p50:-0} ${p90:-0} ${p99:-0} ${p999:-0} ${pmax:-0} ${batch_mean:-0} ${lost:-0}"
}

us() { awk -v n="${1:-0}" 'BEGIN{printf "%.1f", n/1000}'; }

header() {
  echo
  echo "=== $1 ==="
  echo
}

capture_env() {
  local dir="$1"
  mkdir -p "$dir"
  ./scripts/measure-env.sh capture > "$dir/env.txt" 2>/dev/null
  {
    echo "experiment_seconds=$SECONDS_RUN"
    echo "warmup_seconds=$WARMUP"
    echo "binary=$BIN"
  } >> "$dir/env.txt"
}

# ------------------------------------------------------------ experiments ---

exp_latency() {
  local dir="$OUT_ROOT/latency"
  capture_env "$dir"
  header "latency at a fixed offered rate, both topologies"
  printf "%-9s %-8s %10s %10s %10s %10s %10s\n" topology rate p50 p90 p99 p99.9 max | tee "$dir/summary.txt"

  local topo rate r
  for topo in ring queue; do
    for rate in 2000 20000 100000; do
      r=$(run_once "$dir/$topo-$rate" "$topo" "$rate" 256 0 8192 "")
      set -- $r
      printf "%-9s %-8s %9sus %9sus %9sus %9sus %9sus\n" \
        "$topo" "$rate" "$(us "$3")" "$(us "$4")" "$(us "$5")" "$(us "$6")" "$(us "$7")" \
        | tee -a "$dir/summary.txt"
    done
  done
}

exp_linger() {
  local dir="$OUT_ROOT/linger"
  capture_env "$dir"
  header "the batching frontier: what waiting to coalesce buys and costs"
  printf "%-9s %14s %10s %10s %12s\n" linger frames/batch p50 p99 writes/s | tee "$dir/summary.txt"

  local linger r rate=20000
  for linger in 0 100 250 500 1000 2500 5000; do
    r=$(run_once "$dir/linger-$linger" ring "$rate" 256 "$linger" 8192 "")
    set -- $r
    local writes
    writes=$(awk -v rate="$rate" -v b="$8" 'BEGIN{printf "%.0f", (b>0)? rate/b : rate}')
    printf "%-9s %14s %9sus %9sus %12s\n" "${linger}us" "$8" "$(us "$3")" "$(us "$5")" "$writes" \
      | tee -a "$dir/summary.txt"
  done
  echo
  echo "writes/s is the offered rate divided by the mean batch, i.e. the write" | tee -a "$dir/summary.txt"
  echo "syscall rate the coalescing actually achieved." | tee -a "$dir/summary.txt"
}

exp_overload() {
  local dir="$OUT_ROOT/overload"
  capture_env "$dir"
  header "drop rate against offered load, with a consumer that cannot keep up"
  printf "%-9s %12s %12s %12s %10s\n" rate delivered lost "loss %" p99 | tee "$dir/summary.txt"

  local rate r
  for rate in 5000 20000 50000 100000 200000; do
    r=$(run_once "$dir/rate-$rate" ring "$rate" 256 0 8192 300)
    set -- $r
    local frames=$1 lost=$9
    local pct
    pct=$(awk -v f="$frames" -v l="$lost" 'BEGIN{t=f+l; printf "%.2f", (t>0)? 100*l/t : 0}')
    printf "%-9s %12s %12s %11s%% %9sus\n" "$rate" "$frames" "$lost" "$pct" "$(us "$5")" \
      | tee -a "$dir/summary.txt"
  done
  echo
  echo "The consumer stalls 300us per batch throughout, so this measures what the" | tee -a "$dir/summary.txt"
  echo "pipeline does when a consumer is definitively too slow - not how fast the" | tee -a "$dir/summary.txt"
  echo "pipeline can go." | tee -a "$dir/summary.txt"
}

exp_topology() {
  local dir="$OUT_ROOT/topology"
  capture_env "$dir"
  header "topology A against topology B under identical load"
  printf "%-9s %-8s %12s %12s %10s %10s\n" topology sources delivered lost p50 p99 | tee "$dir/summary.txt"

  # Several sources is where the two differ: A must serialise ingest on one
  # thread to keep the sequence ordered, B does not.
  local topo r
  for topo in queue ring; do
    r=$(run_once "$dir/$topo" "$topo" 50000 256 0 8192 200)
    set -- $r
    printf "%-9s %-8s %12s %12s %9sus %9sus\n" "$topo" 1 "$1" "$9" "$(us "$3")" "$(us "$5")" \
      | tee -a "$dir/summary.txt"
  done
}

# ------------------------------------------------------------------ main ---

case "${1:-all}" in
  latency)  exp_latency ;;
  linger)   exp_linger ;;
  overload) exp_overload ;;
  topology) exp_topology ;;
  all)
    exp_latency
    exp_linger
    exp_overload
    exp_topology
    ;;
  *)
    echo "usage: $0 [latency|linger|overload|topology|all]" >&2
    exit 2
    ;;
esac

echo
echo "results and environment written to $OUT_ROOT/"
