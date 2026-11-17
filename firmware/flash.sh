#!/bin/bash
# ============================================================================
# Flash helper for air-quality-sensor.
#
# Usage (deployed boards — location and variant come from device-<name>.yaml):
#   ./flash.sh kueche
#   ./flash.sh wohnzimmer --device 192.168.178.132
#
# Usage (ad-hoc / new boards, legacy):
#   ./flash.sh Schlafzimmer
#   ./flash.sh Schlafzimmer --pm --device 192.168.178.132
#
# Normally you don't need this for deployed boards any more: publish a
# GitHub release and install it from HA (Settings → Updates). flash.sh
# stays for development, first flash, and recovery.
#
# Flags:
#   --pm      Flash the PM variant (air-quality-sensor-pm.yaml) which adds
#             PMSA003I particulate matter support via the QWIIC connector.
#             Without this flag the base firmware is used.
#
# Firmware version logic:
#   - HEAD is exactly on a git tag         → the tag (e.g. "v1.0.0")
#   - HEAD is on a tag with local changes  → tag + "-dirty"
#   - HEAD is NOT on a tag                 → "dev-YYYY-MM-DD" (+ "-dirty")
# ============================================================================

set -e

# --- Location argument (required, first positional arg) ---
LOCATION="${1:-Default}"
shift || true

# --- Parse remaining flags ---
PM_MODE=false
EXTRA_ARGS=()

for arg in "$@"; do
    if [[ "$arg" == "--pm" ]]; then
        PM_MODE=true
    else
        EXTRA_ARGS+=("$arg")
    fi
done

# --- Select firmware file ---
# A known board name (device-<name>.yaml) pins location and variant.
DEVICE_MODE=false
if [ -f "device-${LOCATION}.yaml" ]; then
    DEVICE_MODE=true
    YAML="device-${LOCATION}.yaml"
    grep -q 'air-quality-sensor-pm.yaml' "$YAML" && PM_MODE=true || PM_MODE=false
elif $PM_MODE; then
    YAML="air-quality-sensor-pm.yaml"
else
    YAML="air-quality-sensor.yaml"
fi

# --- Derive firmware version ---
# `git describe --exact-match` succeeds only if HEAD is on a tag.
if TAG=$(git describe --tags --exact-match HEAD 2>/dev/null); then
    VERSION="$TAG"
else
    VERSION="dev-$(date +%Y-%m-%d)"
fi

# Append "-dirty" if there are uncommitted changes.
if ! git diff --quiet HEAD 2>/dev/null || \
   ! git diff --cached --quiet 2>/dev/null; then
    VERSION="${VERSION}-dirty"
fi

# Append "-pm" for the PM variant so the version is visible in Home Assistant.
if $PM_MODE; then
    VERSION="${VERSION}-pm"
fi

echo "Flashing:"
if $DEVICE_MODE; then
    echo "  Board:    $LOCATION (location from $YAML)"
else
    echo "  Location: $LOCATION"
fi
echo "  Firmware: $VERSION"
echo "  Variant:  $YAML"
echo ""

# Board configs have their own build directory each — no cache clash, no
# location override.
if $DEVICE_MODE; then
    exec esphome \
        -s firmware_version "$VERSION" \
        run "$YAML" "${EXTRA_ARGS[@]}"
fi

# --- Auto-clean when switching between base and PM variant ---
# ESPHome reuses the same .pioenvs build cache for both YAMLs (same device
# name). Switching variants without cleaning causes stale component objects
# from the previous build to break compilation.
VARIANT_MARKER=".last_variant"
LAST_VARIANT=""
[ -f "$VARIANT_MARKER" ] && LAST_VARIANT=$(cat "$VARIANT_MARKER")

if [ -n "$LAST_VARIANT" ] && [ "$LAST_VARIANT" != "$YAML" ]; then
    echo "Variant changed ($LAST_VARIANT → $YAML), cleaning build cache..."
    esphome clean "$YAML" 2>/dev/null || true
fi
echo "$YAML" > "$VARIANT_MARKER"

exec esphome \
    -s location "$LOCATION" \
    -s firmware_version "$VERSION" \
    run "$YAML" "${EXTRA_ARGS[@]}"
