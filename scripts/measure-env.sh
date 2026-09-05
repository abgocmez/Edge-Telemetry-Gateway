#!/usr/bin/env bash
# Measurement environment: prepare it, restore it, and record it.
#
#   ./scripts/measure-env.sh capture         provenance record, one key=value per line
#   sudo ./scripts/measure-env.sh perf-on     governor -> performance, saving the old one
#   sudo ./scripts/measure-env.sh perf-off    put the old governor back
#
# `capture` exists because a latency number without its environment is not a
# result. The same binary on the same board reports different tails depending on
# the CPU governor, the die temperature, whether a core is isolated, and how well
# the clock is disciplined - so every published figure has to carry those facts
# with it or it cannot be checked, reproduced, or argued with.
#
# The governor is set per run rather than permanently. An always-performance Pi
# runs hotter and reaches its thermal knee sooner, which would quietly change the
# very throttling behaviour M4 sets out to measure.
set -uo pipefail

STATE_FILE="${TMPDIR:-/tmp}/.etg-governor-saved"

kv() { printf '%s=%s\n' "$1" "${2:-unknown}"; }

first_line() { [ -r "$1" ] && head -1 "$1" 2>/dev/null | tr -d '\0'; }

capture() {
  kv timestamp_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  kv host "$(hostname)"
  kv model "$(first_line /proc/device-tree/model)"
  kv arch "$(uname -m)"
  kv kernel "$(uname -r)"

  if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    kv os "$PRETTY_NAME"
  fi

  kv cores "$(nproc)"
  kv mem_mb "$(awk '/MemTotal/{printf "%d", $2/1024}' /proc/meminfo)"
  kv compiler "$(g++ -dumpversion 2>/dev/null)"

  # Anything that moves the latency tail belongs here.
  kv governor "$(first_line /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  kv cpu_mhz "$(( $(first_line /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))"
  kv isolcpus "$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | grep '^isolcpus=' || echo none)"

  local temp
  temp=$(first_line /sys/class/thermal/thermal_zone0/temp)
  [ -n "$temp" ] && kv temp_c "$(awk -v t="$temp" 'BEGIN{printf "%.1f", t/1000}')"

  if command -v vcgencmd >/dev/null 2>&1; then
    kv throttled "$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)"
  fi

  # Clock discipline. Reported for every run, not just cross-machine ones: a
  # figure quoted without it cannot be defended, and the offset is exactly the
  # error bar on anything measured against another host.
  if command -v chronyc >/dev/null 2>&1 && chronyc tracking >/dev/null 2>&1; then
    kv chrony_stratum "$(chronyc tracking | awk -F': *' '/^Stratum/{print $2}')"
    kv chrony_offset_s "$(chronyc tracking | awk -F': *' '/^Last offset/{print $2}' | awk '{print $1}')"
    kv chrony_rms_s "$(chronyc tracking | awk -F': *' '/^RMS offset/{print $2}' | awk '{print $1}')"
  else
    kv chrony_stratum none
  fi

  local iface
  iface=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
  kv net_iface "${iface:-none}"
  case "$iface" in
    wl*) kv net_kind wireless ;;
    ""|none) kv net_kind none ;;
    *) kv net_kind wired ;;
  esac

  kv storage_fs "$(df -PT "$HOME" 2>/dev/null | awk 'NR==2{print $2}')"
  kv uptime_s "$(awk '{printf "%d", $1}' /proc/uptime)"
  kv load "$(awk '{print $1"/"$2"/"$3}' /proc/loadavg)"
}

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "needs root: sudo $0 $1" >&2
    exit 1
  fi
}

governors() { echo /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; }

perf_on() {
  require_root perf-on
  local current
  current=$(first_line /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
  if [ -z "$current" ]; then
    echo "no cpufreq governor on this system; nothing to do" >&2
    exit 0
  fi
  # Saved rather than assumed, so restore puts back what was actually there.
  if [ ! -f "$STATE_FILE" ]; then
    echo "$current" > "$STATE_FILE"
  fi
  for g in $(governors); do
    echo performance > "$g" 2>/dev/null
  done
  echo "governor: $current -> $(first_line /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) (saved: $(cat "$STATE_FILE"))"
}

perf_off() {
  require_root perf-off
  if [ ! -f "$STATE_FILE" ]; then
    echo "nothing saved; leaving the governor alone" >&2
    exit 0
  fi
  local saved
  saved=$(cat "$STATE_FILE")
  for g in $(governors); do
    echo "$saved" > "$g" 2>/dev/null
  done
  rm -f "$STATE_FILE"
  echo "governor restored to $(first_line /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
}

case "${1:-capture}" in
  capture)  capture ;;
  perf-on)  perf_on ;;
  perf-off) perf_off ;;
  *)
    echo "usage: $0 [capture|perf-on|perf-off]" >&2
    exit 2
    ;;
esac
