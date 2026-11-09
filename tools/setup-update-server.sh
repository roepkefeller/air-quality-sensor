#!/usr/bin/env bash
# ============================================================================
# One-time setup of the firmware update server, inside a fresh Debian LXC
# (Proxmox). Run as root:
#
#   bash <(curl -fsSL https://raw.githubusercontent.com/roepkefeller/air-quality-sensor/main/tools/setup-update-server.sh)
#
# Installs nginx (serves /srv/aqs-firmware on port 80), git, jq, a Python
# venv for ESPHome, the release watcher and its systemd timer (every 10 min).
#
# Afterwards, copy your secrets once from the machine you flash from:
#   scp firmware/secrets.yaml root@aqs-firmware:/etc/aqs-firmware/secrets.yaml
# and publish the current release right away (otherwise the timer does it):
#   aqs-release-watcher
# ============================================================================
set -euo pipefail

REPO="roepkefeller/air-quality-sensor"
BASE=/opt/aqs-firmware

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    nginx git jq curl ca-certificates python3 python3-venv build-essential

install -d -m 755 "$BASE" /srv/aqs-firmware /var/lib/aqs-firmware
install -d -m 700 /etc/aqs-firmware

[ -d "$BASE/repo/.git" ] || git clone --quiet "https://github.com/$REPO.git" "$BASE/repo"
[ -x "$BASE/venv/bin/pip" ] || python3 -m venv "$BASE/venv"
"$BASE/venv/bin/pip" install --quiet --upgrade pip
"$BASE/venv/bin/pip" install --quiet -r "$BASE/repo/tools/requirements-build.txt"

# The watcher is installed from main once; it then checks out each release
# tag and runs that tag's build-release.sh, so build logic always matches
# the firmware being built.
install -m 755 "$BASE/repo/tools/aqs-release-watcher.sh" /usr/local/bin/aqs-release-watcher

cat > /etc/nginx/sites-available/aqs-firmware <<'NGINX'
# Read-only file server for the air-quality-sensor boards (LAN only).
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    root /srv/aqs-firmware;
    autoindex on;
    location ~ /manifest\.json$ { add_header Cache-Control "no-cache"; }
    location / { limit_except GET HEAD { deny all; } }
}
NGINX
ln -sf /etc/nginx/sites-available/aqs-firmware /etc/nginx/sites-enabled/aqs-firmware
rm -f /etc/nginx/sites-enabled/default
nginx -t -q && systemctl reload-or-restart nginx

cat > /etc/systemd/system/aqs-release-watcher.service <<'UNIT'
[Unit]
Description=Build and publish air-quality-sensor firmware for new GitHub releases
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/aqs-release-watcher
# A full build of all boards takes a few minutes on first run (toolchain).
TimeoutStartSec=45min
Nice=10
UNIT

cat > /etc/systemd/system/aqs-release-watcher.timer <<'UNIT'
[Unit]
Description=Check for new air-quality-sensor releases every 10 minutes

[Timer]
OnBootSec=2min
OnUnitActiveSec=10min
Persistent=true

[Install]
WantedBy=timers.target
UNIT

systemctl daemon-reload
systemctl enable --now aqs-release-watcher.timer

echo
echo "Update server ready. Remaining step:"
echo "  scp firmware/secrets.yaml root@$(hostname):/etc/aqs-firmware/secrets.yaml"
echo "Then 'aqs-release-watcher' to publish the latest release now."
echo "Logs: journalctl -u aqs-release-watcher -e"
