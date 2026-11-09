#!/usr/bin/env bash
# ============================================================================
# Create the update-server LXC on the Proxmox host. Run as root on the
# Proxmox node (shell in the web UI or ssh):
#
#   bash create-lxc.sh            # defaults below = the current install
#   VMID=110 bash create-lxc.sh   # rebuild under another ID
#
# Then run setup.sh inside it:
#   pct exec $VMID -- bash -c "apt-get update -qq && apt-get install -y -qq curl && \
#     bash <(curl -fsSL https://raw.githubusercontent.com/roepkefeller/air-quality-sensor/main/tools/update-server/setup.sh)"
#
# Current install (2026-11-09): CT 106 "aqs-firmware" on node pve, Ubuntu
# 24.04, DHCP → 192.168.178.151, reachable as aqs-firmware.fritz.box.
#
# Sizing: an ESPHome/ESP-IDF build peaks at ~1 GB RAM; first build per board
# ~6 min on 2 cores, ~2 min afterwards. ~6 GB disk used after three boards.
# ============================================================================
set -euo pipefail

VMID="${VMID:-106}"
HOSTNAME_CT="${HOSTNAME_CT:-aqs-firmware}"
TEMPLATE="${TEMPLATE:-local:vztmpl/ubuntu-24.04-standard_24.04-2_amd64.tar.zst}"
STORAGE="${STORAGE:-local-lvm}"
BRIDGE="${BRIDGE:-vmbr0}"

pct create "$VMID" "$TEMPLATE" \
    --hostname "$HOSTNAME_CT" \
    --cores 2 --memory 1536 --swap 1024 \
    --rootfs "$STORAGE:16" \
    --net0 "name=eth0,bridge=$BRIDGE,ip=dhcp" \
    --unprivileged 1 --features nesting=1 \
    --onboot 1 --start 1

echo "CT $VMID created and started. Next: setup.sh inside it (see header)."
