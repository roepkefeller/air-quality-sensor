#!/bin/bash
# ============================================================================
# Flash helper for air-quality-sensor.
#
# Usage:
#   ./flash.sh Schlafzimmer
#   ./flash.sh Wohnzimmer --device 192.168.178.132
#
# Firmware version logic:
#   - HEAD is exactly on a git tag         → the tag (e.g. "v1.0.0")
#   - HEAD is on a tag with local changes  → tag + "-dirty"
#   - HEAD is NOT on a tag                 → "dev-YYYY-MM-DD" (+ "-dirty")
# ============================================================================

set -e

# --- Location argument (defaults to "Default") ---
LOCATION="${1:-Default}"
shift || true   # allow passing extra args like --device 192.168.x.x

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

echo "Flashing:"
echo "  Location: $LOCATION"
echo "  Firmware: $VERSION"
echo ""

exec esphome \
    -s location "$LOCATION" \
    -s firmware_version "$VERSION" \
    run air-quality-sensor.yaml "$@"
