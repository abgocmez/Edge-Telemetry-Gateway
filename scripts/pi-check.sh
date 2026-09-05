#!/usr/bin/env bash
# Verifies that the measurement target can do what M4 will ask of it.
#
# Run this once, early. Every finding here is something that would otherwise be
# discovered halfway through a measurement run, with a half-built harness and no
# idea whether the number on screen is the system or the environment. The same
# reasoning put a vcan-availability job in CI on day one, where it immediately
# found two real problems.
#
# Read-only. It installs nothing, loads no modules and changes no settings; it
# reports and suggests. Run it on the Pi:
#
#   git clone <repo> && ./scripts/pi-check.sh
#
# Exit status is 1 if any hard requirement failed.

set -uo pipefail

PASS=0; WARN=0; FAIL=0
NOTES=()

green()  { printf '\033[32m%s\033[0m' "$1"; }
yellow() { printf '\033[33m%s\033[0m' "$1"; }
red()    { printf '\033[31m%s\033[0m' "$1"; }

ok()   { printf '  [%s] %-30s %s\n' "$(green PASS)" "$1" "${2:-}"; PASS=$((PASS+1)); }
warn() { printf '  [%s] %-30s %s\n' "$(yellow WARN)" "$1" "${2:-}"; WARN=$((WARN+1)); NOTES+=("WARN $1: ${3:-${2:-}}"); }
bad()  { printf '  [%s] %-30s %s\n' "$(red FAIL)" "$1" "${2:-}"; FAIL=$((FAIL+1)); NOTES+=("FAIL $1: ${3:-${2:-}}"); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

have() { command -v "$1" >/dev/null 2>&1; }

# modinfo, lsmod and friends live in /sbin and /usr/sbin, which are not on a
# regular user's PATH on Debian. Without this the module check reports a missing
# module when the module is present - a false FAIL, and the first thing this
# script itself got wrong.
export PATH="$PATH:/sbin:/usr/sbin"

# ---------------------------------------------------------------- platform ---
head_ "Platform"

arch=$(uname -m)
if [ "$arch" = "aarch64" ]; then
  ok "architecture" "$arch"
else
  # On armv7 std::atomic<uint64_t> lock-freedom needs checking and the 64-bit
  # sequence arithmetic gets awkward - both load-bearing for the M2 ring.
  bad "architecture" "$arch (want aarch64)" \
      "install the 64-bit Raspberry Pi OS; armv7 changes the atomics story"
fi

if [ -r /etc/os-release ]; then
  . /etc/os-release
  ok "os" "$PRETTY_NAME"
else
  warn "os" "unknown"
fi

ok "kernel" "$(uname -r)"

model=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)
[ -n "$model" ] && ok "model" "$model"

cores=$(nproc)
if [ "$cores" -ge 4 ]; then
  ok "cores" "$cores"
else
  warn "cores" "$cores" "the thread budget will be tighter than planned"
fi

mem_mb=$(awk '/MemTotal/{printf "%d", $2/1024}' /proc/meminfo)
if [ "$mem_mb" -ge 900 ]; then
  ok "memory" "${mem_mb} MB"
else
  warn "memory" "${mem_mb} MB" "ring sizing and mlockall need care"
fi

# -------------------------------------------------------------------- can ---
head_ "SocketCAN"

if modinfo vcan >/dev/null 2>&1; then
  ok "vcan module" "available"
elif lsmod 2>/dev/null | grep -q '^vcan'; then
  ok "vcan module" "already loaded"
else
  bad "vcan module" "not found" \
      "sudo apt install linux-modules-extra-\$(uname -r), or check the kernel config"
fi

if [ -e /usr/include/linux/can.h ]; then
  ok "can headers" "linux/can.h"
else
  bad "can headers" "missing" "sudo apt install linux-libc-dev"
fi

if have cansend && have candump; then
  ok "can-utils" "present"
else
  warn "can-utils" "missing" "sudo apt install can-utils (needed by the smoke tests)"
fi

# -------------------------------------------------------------- toolchain ---
head_ "Toolchain"

if have g++; then
  gccver=$(g++ -dumpversion | cut -d. -f1)
  if [ "$gccver" -ge 12 ]; then
    ok "g++" "$(g++ --version | head -1)"
  else
    bad "g++" "version $gccver" "C++20 needs GCC 12 or newer"
  fi
else
  bad "g++" "missing" "sudo apt install build-essential"
fi

for t in cmake ninja git; do
  if have "$t"; then
    ok "$t" "$($t --version 2>/dev/null | head -1)"
  else
    bad "$t" "missing" "sudo apt install cmake ninja-build git"
  fi
done

if have docker; then
  ok "docker" "$(docker --version 2>/dev/null)"
else
  warn "docker" "missing" "only needed for the compose path; native builds work without it"
fi

# ------------------------------------------------------------------ clock ---
head_ "Clocks"

if have python3; then
  res=$(python3 - <<'PY' 2>/dev/null
import time
r = time.clock_getres(time.CLOCK_MONOTONIC)
print(f"{r*1e9:.0f} ns")
PY
)
  if [ -n "$res" ]; then
    ok "CLOCK_MONOTONIC resolution" "$res"
  else
    warn "CLOCK_MONOTONIC resolution" "unreadable"
  fi
else
  warn "python3" "missing" "needed by parse-capture.py and the smoke tests"
fi

if have chronyc; then
  if chronyc tracking >/dev/null 2>&1; then
    offset=$(chronyc tracking 2>/dev/null | awk -F': *' '/System time/{print $2}')
    ok "chrony" "running, ${offset:-unknown}"
  else
    warn "chrony" "installed but not tracking" "needed for cross-machine latency in M3/M4"
  fi
else
  warn "chrony" "missing" "sudo apt install chrony; required before any cross-machine number"
fi

# Only meaningful once chrony is disciplining the clock, but worth surfacing
# early: a step during a run silently corrupts every sample in it.
if [ -r /etc/chrony/chrony.conf ]; then
  if grep -qE '^\s*makestep' /etc/chrony/chrony.conf; then
    warn "chrony makestep" "enabled" \
         "disable it during measurement runs; a step mid-run corrupts the samples"
  else
    ok "chrony makestep" "not configured"
  fi
fi

# -------------------------------------------------------------- realtime ----
head_ "Real-time and thermal"

if grep -q isolcpus /proc/cmdline 2>/dev/null; then
  ok "isolcpus" "$(tr ' ' '\n' < /proc/cmdline | grep isolcpus)"
else
  warn "isolcpus" "not set" \
       "needed for the SCHED_FIFO comparison; add to /boot/firmware/cmdline.txt"
fi

if [ -r /sys/class/thermal/thermal_zone0/temp ]; then
  temp=$(awk '{printf "%.1f C", $1/1000}' /sys/class/thermal/thermal_zone0/temp)
  ok "thermal sensor" "$temp"
else
  warn "thermal sensor" "unreadable" "the throttling curve cannot be recorded without it"
fi

if have vcgencmd; then
  thr=$(vcgencmd get_throttled 2>/dev/null)
  if [ "$thr" = "throttled=0x0" ]; then
    ok "throttling" "$thr (clean)"
  else
    warn "throttling" "$thr" "already throttled or under-volted; check the power supply first"
  fi
else
  warn "vcgencmd" "missing" "throttle state cannot be read; sudo apt install libraspberrypi-bin"
fi

if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
  if [ "$gov" = "performance" ]; then
    ok "cpu governor" "$gov"
  else
    warn "cpu governor" "$gov" \
         "ondemand adds frequency-ramp latency to the tail; set performance for measurement runs"
  fi
fi

# ---------------------------------------------------------------- network ---
head_ "Network"

route_if=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
if [ -n "$route_if" ]; then
  case "$route_if" in
    wl*)
      warn "default route" "$route_if (wireless)" \
           "fine as a management link; use Ethernet for any cross-machine measurement - \
wireless jitter is milliseconds and will swamp a 40us signal, and it degrades chrony too"
      ;;
    eth*|en*)
      ok "default route" "$route_if (wired)"
      ;;
    *)
      ok "default route" "$route_if"
      ;;
  esac
else
  warn "default route" "none"
fi

if have iperf3; then
  ok "iperf3" "present"
else
  warn "iperf3" "missing" \
       "sudo apt install iperf3; M4 must establish the link ceiling before publishing throughput"
fi

# ------------------------------------------------------------------- disk ---
head_ "Storage"

# Measured where the recorder would actually write, not in whatever directory
# this happens to be launched from. Run in /tmp on a Pi this reported 439 MB/s -
# impossible over USB 2.0, and simply the speed of RAM, because /tmp is a tmpfs.
# That is the same class of mistake as reporting latency without excluding
# warm-up, and this script made it before it caught it.
WRITE_DIR="${PI_CHECK_WRITE_DIR:-$HOME}"
fstype=$(df -PT "$WRITE_DIR" 2>/dev/null | awk 'NR==2{print $2}')
mountpt=$(df -P "$WRITE_DIR" 2>/dev/null | awk 'NR==2{print $6}')

if [ "$fstype" = "tmpfs" ] || [ "$fstype" = "ramfs" ]; then
  warn "write target" "$WRITE_DIR is $fstype" \
       "that is RAM, not storage; set PI_CHECK_WRITE_DIR to a real filesystem"
else
  ok "write target" "$WRITE_DIR ($fstype on $mountpt)"

  tmpf="$WRITE_DIR/.pi-check-write-test"
  speed=$(dd if=/dev/zero of="$tmpf" bs=1M count=64 conv=fsync 2>&1 | tail -1 | sed 's/.*, //')
  rm -f "$tmpf"
  if [ -n "$speed" ]; then
    ok "sequential write" "$speed"
    NOTES+=("NOTE storage: $speed on $fstype - this is the recorder's real write speed. On WSL the same measurement reported 125ns per write because it never left the page cache.")
  else
    warn "sequential write" "could not measure"
  fi
fi

# ---------------------------------------------------------------- summary ---
head_ "Summary"
printf '  %s %d   %s %d   %s %d\n\n' "$(green PASS)" "$PASS" "$(yellow WARN)" "$WARN" "$(red FAIL)" "$FAIL"

if [ ${#NOTES[@]} -gt 0 ]; then
  for n in "${NOTES[@]}"; do
    printf '  - %s\n' "$n"
  done
  echo
fi

if [ "$FAIL" -gt 0 ]; then
  echo "  Hard requirements are unmet. M4 numbers taken here would not mean what they claim."
  exit 1
fi

echo "  Ready for M4 measurement, subject to the warnings above."
