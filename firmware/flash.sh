#!/bin/bash
# ============================================================================
# Flash helper for air-quality-sensor.
#
# Usage:
#   ./flash.sh Schlafzimmer
#   ./flash.sh Wohnzimmer --device 192.168.178.132
#   ./flash.sh Schlafzimmer --pm
#   ./flash.sh Schlafzimmer --pm --device 192.168.178.132
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
if $PM_MODE; then
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
echo "  Location: $LOCATION"
echo "  Firmware: $VERSION"
echo "  Variant:  $YAML"
echo ""

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
