#!/usr/bin/env bash
# ============================================================================
# Runs on the update server (systemd timer, every 10 min). When the latest
# GitHub release of the repo changes, checks out that tag, builds all
# boards and publishes them for the boards' update entities.
#
#   aqs-release-watcher            # normal run (what the timer does)
#   aqs-release-watcher v1.1.0     # (re)publish a specific tag, e.g. rollback
#
# Drafts and pre-releases are ignored by GitHub's /releases/latest, so a
# pre-release is a safe way to tag something without offering it to the
# boards yet; publish a specific tag by hand to test it.
#
# Pulls from GitHub only — nothing on this server is reachable from outside,
# and no GitHub runner or token is involved. Secrets stay in
# /etc/aqs-firmware/secrets.yaml and are copied into the build checkout.
# ============================================================================
set -euo pipefail

REPO="${AQS_REPO:-roepkefeller/air-quality-sensor}"
CHECKOUT="${AQS_CHECKOUT:-/opt/aqs-firmware/repo}"
WEBROOT="${AQS_WEBROOT:-/srv/aqs-firmware}"
SECRETS="${AQS_SECRETS:-/etc/aqs-firmware/secrets.yaml}"
STATE="${AQS_STATE:-/var/lib/aqs-firmware/published_tag}"
VENV="${AQS_VENV:-/opt/aqs-firmware/venv}"

api() { curl -fsSL -H "Accept: application/vnd.github+json" "https://api.github.com/repos/$REPO/$1"; }

if [ $# -ge 1 ]; then
    release_json="$(api "releases/tags/$1")"
else
    release_json="$(api releases/latest)" || { echo "no published release yet"; exit 0; }
fi

tag="$(jq -r .tag_name <<<"$release_json")"
url="$(jq -r .html_url <<<"$release_json")"
# HA summary: release title, else first non-empty line of the notes.
summary="$(jq -r '(.name // "") as $n | if ($n|length) > 0 then $n
                  else ((.body // "") | split("\n") | map(select(length>0)) | .[0] // "") end' <<<"$release_json")"

published="$(cat "$STATE" 2>/dev/null || true)"
if [ $# -eq 0 ] && [ "$tag" = "$published" ]; then
    exit 0
fi
echo "Publishing $tag (currently published: ${published:-none})"

if [ ! -d "$CHECKOUT/.git" ]; then
    git clone --quiet "https://github.com/$REPO.git" "$CHECKOUT"
fi
git -C "$CHECKOUT" fetch --quiet --force --tags origin
git -C "$CHECKOUT" checkout --quiet --force "refs/tags/$tag"
git -C "$CHECKOUT" clean --quiet -fdx -e firmware/.esphome

install -m 600 "$SECRETS" "$CHECKOUT/firmware/secrets.yaml"

# Build with the ESPHome version pinned in the tagged tree.
"$VENV/bin/pip" install --quiet -r "$CHECKOUT/tools/requirements-build.txt"

ESPHOME="$VENV/bin/esphome" \
    "$CHECKOUT/tools/build-release.sh" "$tag" "$WEBROOT" "$url" "${summary:-Firmware $tag}"

mkdir -p "$(dirname "$STATE")"
echo "$tag" > "$STATE"
echo "Done: $tag is now offered to the boards (install from HA → Settings → Updates)."
