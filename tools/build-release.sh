#!/usr/bin/env bash
# ============================================================================
# Build every board config (firmware/device-*.yaml) and publish the OTA
# images + manifests to the update server's web root.
#
# Usage:
#   tools/build-release.sh <version> <webroot> [release_url] [summary]
#
#   tools/build-release.sh v1.2.0 /srv/aqs-firmware \
#       https://github.com/roepkefeller/air-quality-sensor/releases/tag/v1.2.0 \
#       "LED pulse no longer stutters"
#
# Normally run by tools/update-server/aqs-release-watcher.sh on the update server, from a
# checkout of the release tag, with firmware/secrets.yaml in place.
#
# Result per board (served by nginx):
#   <webroot>/<slug>/manifest.json
#   <webroot>/<slug>/firmware-<version>.ota.bin
#
# The boards poll their manifest (see `update:` in air-quality-sensor.yaml)
# and HA offers the update when the manifest version differs from theirs.
# All boards are built before anything is published, so a compile error in
# one config never leaves the boards on mixed versions.
# ============================================================================
set -euo pipefail

VERSION="${1:?usage: build-release.sh <version> <webroot> [release_url] [summary]}"
WEBROOT="${2:?usage: build-release.sh <version> <webroot> [release_url] [summary]}"
RELEASE_URL="${3:-}"
SUMMARY="${4:-Firmware ${VERSION}}"
ESPHOME="${ESPHOME:-esphome}"
KEEP_IMAGES=3   # older images kept per board, for quick rollback

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT/firmware"

[ -f secrets.yaml ] || { echo "firmware/secrets.yaml missing" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

shopt -s nullglob
configs=(device-*.yaml)
[ ${#configs[@]} -gt 0 ] || { echo "no firmware/device-*.yaml found" >&2; exit 1; }

for cfg in "${configs[@]}"; do
    slug="${cfg#device-}"; slug="${slug%.yaml}"

    # Same version string flash.sh would stamp, so a board flashed by hand
    # with the same tag isn't offered the same release again.
    ver="$VERSION"
    if grep -q 'air-quality-sensor-pm.yaml' "$cfg"; then ver="${VERSION}-pm"; fi

    echo "=== Building $slug ($ver)"
    "$ESPHOME" -s firmware_version "$ver" compile "$cfg"

    bin=".esphome/build/aqs-${slug}/.pioenvs/air-quality-sensor/firmware.ota.bin"
    [ -f "$bin" ] || { echo "OTA image not found: $bin" >&2; exit 1; }

    mkdir -p "$STAGE/$slug"
    img="firmware-${ver}.ota.bin"
    cp "$bin" "$STAGE/$slug/$img"
    md5="$(md5sum "$STAGE/$slug/$img" | cut -d' ' -f1)"

    # Manifest format read by ESPHome's http_request update platform.
    # "name" shows up as the update title in HA, "summary" as the release
    # summary, "release_url" as the "Release notes" link.
    VER="$ver" IMG="$img" MD5="$md5" URL="$RELEASE_URL" SUM="$SUMMARY" \
    python3 - > "$STAGE/$slug/manifest.json" <<'PY'
import json, os
ota = {"path": os.environ["IMG"], "md5": os.environ["MD5"],
       "summary": os.environ["SUM"][:250]}
if os.environ["URL"]:
    ota["release_url"] = os.environ["URL"]
print(json.dumps({
    "name": "Luftqualitätssensor Firmware",
    "version": os.environ["VER"],
    "home_assistant_domain": "esphome",
    "builds": [{"chipFamily": "ESP32-C3", "ota": ota}],
}, ensure_ascii=False, indent=2))
PY
done

# --- Publish: image first, manifest last (atomic rename) ---
for dir in "$STAGE"/*/; do
    slug="$(basename "$dir")"
    dest="$WEBROOT/$slug"
    mkdir -p "$dest"
    cp "$dir"/*.ota.bin "$dest/"
    cp "$dir/manifest.json" "$dest/.manifest.json.tmp"
    mv -f "$dest/.manifest.json.tmp" "$dest/manifest.json"
    # Prune old images, keep the newest $KEEP_IMAGES.
    ls -1t "$dest"/firmware-*.ota.bin 2>/dev/null | tail -n +$((KEEP_IMAGES + 1)) | xargs -r rm -f
    echo "Published $slug → $dest/manifest.json"
done
