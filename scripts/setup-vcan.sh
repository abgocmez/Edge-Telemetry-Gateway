#!/usr/bin/env bash
# Create N virtual CAN interfaces (vcan0..vcanN-1). Idempotent.
#
# vcan lives in the host network namespace and needs a host kernel module, so the
# gateway container runs with network_mode: host and NET_ADMIN.
#
# Not available on the default WSL2 kernel (CONFIG_CAN_VCAN is not set there).
# Use SyntheticSource for local development; this script is for CI and the Pi.
set -euo pipefail

COUNT="${1:-1}"

if ! lsmod | grep -q '^vcan'; then
  sudo modprobe vcan
fi

for i in $(seq 0 $((COUNT - 1))); do
  dev="vcan${i}"
  if ip link show "$dev" >/dev/null 2>&1; then
    echo "$dev already exists"
  else
    sudo ip link add dev "$dev" type vcan
    echo "$dev created"
  fi
  sudo ip link set up "$dev"
done

ip -brief link show type vcan
