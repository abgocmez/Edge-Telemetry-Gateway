#!/usr/bin/env bash
# Runs a named experiment and writes its results, raw output and environment to
# results/<host>-<date>/<experiment>/.
#
#   ./scripts/measure.sh latency
#   ./scripts/measure.sh linger
#   ./scripts/measure.sh overload
#   ./scripts/measure.sh topology
#   ./scripts/measure.sh sched
#   ./scripts/measure.sh recovery
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

# This script runs without set -e, because an individual experiment failing
# should not abandon the rest of the suite. That makes an unchecked cd
# dangerous rather than merely untidy: it would carry on in the wrong
# directory and write results somewhere nobody looks.
cd "$(cd "$(dirname "$0")/.." && pwd)" || exit 1

BIN="${MEASURE_BIN:-build/src}"
SECONDS_RUN="${MEASURE_SECONDS:-12}"
WARMUP="${MEASURE_WARMUP:-3}"
PORT="${MEASURE_PORT:-9900}"
OUT_ROOT="${MEASURE_OUT:-results/$(hostname)-$(date +%Y%m%d)}"

# Prepended to both the gateway and the probe, so an experiment can change how
# they are scheduled without changing what they do. Empty for every experiment
# but sched.
LAUNCH=()

# Appended to both, for a flag that has to reach the process itself rather than
# its launcher. Also sched-only.
EXTRA=()

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
  local sources="${8:-1}"
  mkdir -p "$run_dir"

  local src_args=()
  local i
  for ((i = 0; i < sources; i++)); do
    src_args+=(--source synth)
  done

  "${LAUNCH[@]}" "$BIN/etg-gateway" --topology "$topology" "${src_args[@]}" --rate "$rate" \
    --capacity "$capacity" --batch "$batch" --linger-us "$linger" \
    --consumer probe:"$PORT" --seconds "$((SECONDS_RUN + 2))" "${EXTRA[@]}" \
    >/dev/null 2>"$run_dir/gateway.log" &
  local gw=$!
  sleep 1

  "${LAUNCH[@]}" "$BIN/etg-probe" --port "$PORT" --seconds "$SECONDS_RUN" --warmup "$WARMUP" \
    ${stall:+--stall-us "$stall"} "${EXTRA[@]}" >/dev/null 2>"$run_dir/probe.log"
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
  printf "%-9s %-8s %12s %12s %11s\n" topology sources "gw sent" "gw dropped" "delivered %" \
    | tee "$dir/summary.txt"

  # The source count is the variable that matters, and an earlier version of this
  # experiment left it at one - where the two topologies do almost the same work
  # and the comparison says almost nothing.
  #
  # With several sources, A has to poll them all from a single thread and copy
  # every frame once per consumer. That single thread is forced rather than
  # chosen: order in a queue is push order, so spreading ingest across threads
  # would hand a consumer sequences out of order. B takes its order from the cell
  # position and has no such constraint.
  local topo srcs r
  for srcs in 1 4; do
    for topo in queue ring; do
      run_once "$dir/$topo-$srcs" "$topo" 25000 256 0 8192 200 "$srcs" >/dev/null

      # Gateway-side counters, not the consumer's.
      #
      # The consumer stalls on purpose here, which makes its latency confounded:
      # a topology that delivers more also delivers older frames, so a lower p50
      # can mean "dropped more" rather than "was faster". What the gateway sent
      # and dropped is unambiguous. The probe logs still hold the rest.
      local gwlog="$dir/$topo-$srcs/gateway.log"
      local sent dropped pct
      sent=$(tail -1 "$gwlog" | tr ' ' '\n' | sed -n 's/^sent=//p')
      dropped=$(tail -1 "$gwlog" | tr ' ' '\n' | sed -n 's/^dropped=//p')
      pct=$(awk -v s="${sent:-0}" -v d="${dropped:-0}" \
        'BEGIN{t=s+d; printf "%.1f", (t>0)? 100*s/t : 0}')
      printf "%-9s %-8s %12s %12s %10s%%\n" "$topo" "$srcs" "${sent:-0}" "${dropped:-0}" "$pct" \
        | tee -a "$dir/summary.txt"
    done
  done
  {
    echo
    echo "Rate is per source, so the four-source rows offer four times the load."
    echo
    echo "Consumer-side latency is deliberately not compared here, for the reason"
    echo "in the comment above: under a deliberate stall, delivering more also"
    echo "means delivering older, and the two effects are not separable."
  } | tee -a "$dir/summary.txt"
}

# Busy loops at ordinary priority, one per core, to make the scheduler choose.
# With no contention every policy looks identical and the experiment says
# nothing.
LOAD_PIDS=()
start_load() {
  local n i
  n="$(nproc)"
  for ((i = 0; i < n; i++)); do
    sh -c 'while :; do :; done' &
    LOAD_PIDS+=("$!")
  done
  sleep 1
}

stop_load() {
  local pid
  for pid in "${LOAD_PIDS[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  LOAD_PIDS=()
  sleep 1
}

exp_sched() {
  local dir="$OUT_ROOT/sched"
  capture_env "$dir"
  header "scheduling policy under CPU contention"

  if ! command -v chrt >/dev/null 2>&1; then
    echo "chrt not found (install util-linux); skipping" | tee "$dir/summary.txt"
    return
  fi
  if ! sudo -n true 2>/dev/null; then
    echo "SCHED_FIFO needs sudo and none is available without a password; skipping"       | tee "$dir/summary.txt"
    return
  fi

  printf "%-16s %10s %10s %10s %10s %10s %10s
" arm p50 p90 p99 p99.9 max lost     | tee "$dir/summary.txt"

  local arm out
  for arm in idle-other loaded-other loaded-fifo loaded-fifo-mlock; do
    case "$arm" in
      idle-other)   LAUNCH=(); EXTRA=() ;;
      loaded-other) LAUNCH=(); EXTRA=(); start_load ;;
      loaded-fifo)  LAUNCH=(sudo chrt -f 20 --); EXTRA=() ;;
      # Priority settles who runs among the runnable. A thread waiting on a page
      # that is not resident is not runnable, and no priority shortens that wait.
      loaded-fifo-mlock) LAUNCH=(sudo chrt -f 20 --); EXTRA=(--mlock) ;;
    esac

    out=$(run_once "$dir/$arm" ring 20000 256 100 8192 "")
    printf "%-16s %10s %10s %10s %10s %10s %10s
" "$arm"       "$(us "$(echo "$out" | cut -d" " -f3)")"       "$(us "$(echo "$out" | cut -d" " -f4)")"       "$(us "$(echo "$out" | cut -d" " -f5)")"       "$(us "$(echo "$out" | cut -d" " -f6)")"       "$(us "$(echo "$out" | cut -d" " -f7)")"       "$(echo "$out" | cut -d" " -f9)" | tee -a "$dir/summary.txt"

    [ "$arm" = loaded-fifo-mlock ] && stop_load
  done
  LAUNCH=()

  {
    echo
    echo "Microseconds, and frames lost. One busy loop per core at ordinary"
    echo "priority in the two loaded arms, so the scheduler has to choose between"
    echo "the pipeline and work that never yields."
    echo
    echo "The gateway and the probe get the same policy in each arm. That is"
    echo "deliberate: the probe is the instrument, and promoting the gateway while"
    echo "leaving the probe at ordinary priority would measure how long the"
    echo "instrument waited to be run. It also means every figure here includes"
    echo "the probe scheduling delay, in every arm, and is an upper bound on what"
    echo "the pipeline itself contributes."
    echo
    echo "No pinning. The supervisor isolcpus result came from a single-threaded"
    echo "periodic loop, which is exactly the shape one isolated core suits. This"
    echo "pipeline runs several ingest and egress threads, and confining them all"
    echo "to one core would serialise work meant to overlap: a different"
    echo "experiment, with a predictable and uninteresting answer."
  } | tee -a "$dir/summary.txt"
}

exp_recovery() {
  local dir="$OUT_ROOT/recovery"
  capture_env "$dir"
  header "what a consumer costs when it dies, and what it costs everyone else"

  local cycles="${RECOVERY_CYCLES:-10}"
  local outage="${RECOVERY_OUTAGE:-2}"
  local rate=20000
  local victim_port=$((PORT + 1))
  local witness_port=$((PORT + 2))
  mkdir -p "$dir"

  # A second consumer that is never touched. The claim being tested is not just
  # that the victim comes back, but that its death is invisible to everyone
  # else - which is the whole reason for per-consumer egress threads and
  # independent cursors.
  "$BIN/etg-gateway" --topology ring --source synth --rate "$rate"     --capacity 8192 --batch 256 --linger-us 100     --consumer victim:"$victim_port" --consumer witness:"$witness_port"     --seconds $(( cycles * (2 + outage) + 8 ))     >/dev/null 2>"$dir/gateway.log" &
  local gw=$!
  sleep 1

  "$BIN/etg-probe" --port "$witness_port" --reconnect     --seconds $(( cycles * (2 + outage) + 4 )) --warmup 1     >/dev/null 2>"$dir/witness.log" &
  local witness=$!

  local i
  for ((i = 1; i <= cycles; i++)); do
    "$BIN/etg-probe" --port "$victim_port" --seconds 30 --warmup 0       >/dev/null 2>"$dir/victim-$i.log" &
    local victim=$!
    sleep 2
    # SIGKILL, not SIGTERM: a consumer that got the chance to shut down tidily
    # is not the case worth measuring.
    kill -9 "$victim" 2>/dev/null
    wait "$victim" 2>/dev/null
    sleep "$outage"
  done

  wait "$witness" 2>/dev/null
  wait "$gw" 2>/dev/null

  printf "%-26s %s\n" "cycles" "$cycles kills, ${outage}s outage each" | tee "$dir/summary.txt"

  # From the per-connection lines the victims print as it happens. A SIGKILLed
  # process never reaches its summary, so anything only reported there would be
  # missing for exactly the case being measured.
  local ttf n p50 pmin pmax
  ttf=$(sed -n 's/^first frame \([0-9]*\)ns after connect/\1/p' "$dir"/victim-*.log | sort -n)
  n=$(echo "$ttf" | grep -c .)
  pmin=$(echo "$ttf" | head -1)
  pmax=$(echo "$ttf" | tail -1)
  p50=$(echo "$ttf" | awk '{v[NR]=$1} END{if(NR)print v[int((NR+1)/2)]}')
  printf "%-26s p50 %sus, min %sus, max %sus (n=%s)\n" "connect to first frame" \
    "$(us "${p50:-0}")" "$(us "${pmin:-0}")" "$(us "${pmax:-0}")" "$n" \
    | tee -a "$dir/summary.txt"

  # The gateway's own counters, which are authoritative about what it did.
  local v_line w_line
  v_line=$(grep '^consumer victim' "$dir/gateway.log" | tail -1)
  w_line=$(grep '^consumer witness' "$dir/gateway.log" | tail -1)
  local v_conn v_disc v_gaps
  v_conn=$(echo "$v_line" | sed -n 's/.*connects=\([0-9]*\).*/\1/p')
  v_disc=$(echo "$v_line" | sed -n 's/.*disconnects=\([0-9]*\).*/\1/p')
  v_gaps=$(echo "$v_line" | sed -n 's/.*gaps_sent=\([0-9]*\).*/\1/p')
  printf "%-26s connects=%s disconnects=%s markers_sent=%s\n" "victim, per the gateway" \
    "${v_conn:-?}" "${v_disc:-?}" "${v_gaps:-?}" | tee -a "$dir/summary.txt"

  local w_lost w_silent w_conn w_gaps
  w_lost=$(sed -n 's/^markers  *[0-9][0-9]*  *(\([0-9][0-9]*\) frames.*/\1/p' "$dir/witness.log")
  w_silent=$(sed -n 's/^silent jumps  *\([0-9][0-9]*\).*/\1/p' "$dir/witness.log")
  w_conn=$(sed -n 's/^connections  *\([0-9][0-9]*\).*/\1/p' "$dir/witness.log")
  w_gaps=$(echo "$w_line" | sed -n 's/.*gaps_sent=\([0-9]*\).*/\1/p')
  printf "%-26s lost=%s silent_jumps=%s connections=%s markers_sent=%s\n" "witness, untouched" \
    "${w_lost:-?}" "${w_silent:-?}" "${w_conn:-?}" "${w_gaps:-?}" | tee -a "$dir/summary.txt"

  {
    echo
    echo "The victim is SIGKILLed ten times with a two second outage between"
    echo "each, while the witness consumer on another port is never touched."
    echo
    echo "Connect-to-first-frame is timed from the connection being established,"
    echo "not from process start: exec and dynamic linking belong to whatever"
    echo "restarts the consumer, and charging them to the gateway would measure"
    echo "the wrong thing."
    echo
    echo "The witness line is the one that matters. A gateway that stalls, drops"
    echo "or resequences for its other consumers when one of them dies has not"
    echo "isolated them, whatever the recovery time says."
  } | tee -a "$dir/summary.txt"
}

# ------------------------------------------------------------------ main ---

case "${1:-all}" in
  latency)  exp_latency ;;
  linger)   exp_linger ;;
  overload) exp_overload ;;
  topology) exp_topology ;;
  sched)    exp_sched ;;
  recovery) exp_recovery ;;
  all)
    exp_latency
    exp_linger
    exp_overload
    exp_topology
    exp_sched
    exp_recovery
    ;;
  *)
    echo "usage: $0 [latency|linger|overload|topology|sched|recovery|all]" >&2
    exit 2
    ;;
esac

echo
echo "results and environment written to $OUT_ROOT/"
