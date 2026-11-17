#!/usr/bin/env bash
# ============================================================================
# Set up (or update) the firmware update server inside the LXC. Run as root:
#
#   bash <(curl -fsSL https://raw.githubusercontent.com/roepkefeller/air-quality-sensor/main/tools/update-server/setup.sh)
#
# Idempotent: re-run it after changing anything in tools/update-server/ to
# bring the server in line with the repo.
#
# Everything the server runs comes from this folder:
#   aqs-firmware.nginx.conf        → /etc/nginx/sites-available/aqs-firmware
#   aqs-release-watcher.sh         → /usr/local/bin/aqs-release-watcher
#   aqs-release-watcher.service    → /etc/systemd/system/
#   aqs-release-watcher.timer      → /etc/systemd/system/
# plus tools/build-release.sh and tools/requirements-build.txt, which the
# watcher runs from the checked-out release tag.
#
# Not in the repo (by design): /etc/aqs-firmware/secrets.yaml. Copy it once:
#   scp firmware/secrets.yaml root@aqs-firmware.fritz.box:/etc/aqs-firmware/secrets.yaml
# ============================================================================
set -euo pipefail

REPO="roepkefeller/air-quality-sensor"
BRANCH="${AQS_SETUP_BRANCH:-main}"
BASE=/opt/aqs-firmware
SRC="$BASE/setup-src"

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    nginx git jq curl ca-certificates python3 python3-venv build-essential

install -d -m 755 "$BASE" /srv/aqs-firmware /var/lib/aqs-firmware
install -d -m 700 /etc/aqs-firmware

# Setup files come from a checkout of $BRANCH. The watcher keeps its own
# checkout ($BASE/repo) on release tags, so the two never fight.
if [ -d "$SRC/.git" ]; then
    git -C "$SRC" fetch --quiet origin "$BRANCH"
    git -C "$SRC" checkout --quiet --force FETCH_HEAD
else
    git clone --quiet --branch "$BRANCH" "https://github.com/$REPO.git" "$SRC"
fi
[ -d "$BASE/repo/.git" ] || git clone --quiet "https://github.com/$REPO.git" "$BASE/repo"

D="$SRC/tools/update-server"

[ -x "$BASE/venv/bin/pip" ] || python3 -m venv "$BASE/venv"
"$BASE/venv/bin/pip" install --quiet --upgrade pip
"$BASE/venv/bin/pip" install --quiet -r "$SRC/tools/requirements-build.txt"

install -m 755 "$D/aqs-release-watcher.sh" /usr/local/bin/aqs-release-watcher

install -m 644 "$D/aqs-firmware.nginx.conf" /etc/nginx/sites-available/aqs-firmware
ln -sf /etc/nginx/sites-available/aqs-firmware /etc/nginx/sites-enabled/aqs-firmware
rm -f /etc/nginx/sites-enabled/default
nginx -t -q && systemctl reload-or-restart nginx

install -m 644 "$D/aqs-release-watcher.service" "$D/aqs-release-watcher.timer" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now aqs-release-watcher.timer

echo
if [ -f /etc/aqs-firmware/secrets.yaml ]; then
    echo "Update server ready (setup from $BRANCH @ $(git -C "$SRC" rev-parse --short HEAD))."
else
    echo "Update server installed. Missing: /etc/aqs-firmware/secrets.yaml"
    echo "  scp firmware/secrets.yaml root@$(hostname).fritz.box:/etc/aqs-firmware/secrets.yaml"
fi
echo "Publish the latest release now: /usr/local/bin/aqs-release-watcher"
echo "Logs: journalctl -u aqs-release-watcher -e"
