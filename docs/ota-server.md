# Firmware updates over the air

Publish a GitHub release → within ~10 minutes Home Assistant shows
**"Update available"** for every board (Settings → Updates), like any other
update. Click **Install** per board (or select several). Nothing installs on
its own.

```
GitHub release ──(poll, 10 min)──► update server (Proxmox LXC)
                                    builds device-*.yaml with your secrets
                                    nginx: /<board>/manifest.json + .ota.bin
                                              ▲
boards poll their own manifest every 6 h ─────┘──► HA update entity "Firmware"
```

## What stays per board

Each board has its own config in `firmware/` that fixes room and variant:

| Board  | MAC suffix | Config                    | Variant      |
|--------|------------|---------------------------|--------------|
| Küche  | b7f8c0     | `device-kueche.yaml`      | base + PM    |
| Schlafzimmer | b7f8b0 | `device-schlafzimmer.yaml` | base       |
| Wohnzimmer | b7f8b8 | `device-wohnzimmer.yaml`  | base         |

Every board fetches only its own manifest (`/<slug>/manifest.json`), so the
Küche image (with the particulate sensor) can't land on another board.
Thresholds, LED brightness and other settings are stored on the board and
survive updates. Küche is built as `location: Kueche` because that's how it
was first flashed (the "Küche" display name is a rename in HA).

New board: copy a `device-*.yaml`, set `location` and `device_slug`, flash
once with `./flash.sh <slug>`, and from then on it updates with the rest.

## Release flow

1. Merge to `main`, then create a release on GitHub with a tag like `v1.2.0`.
   The release title becomes the summary shown in HA; the "Release notes"
   link points to the GitHub release.
2. The update server picks it up, builds all boards (a few minutes) and
   publishes. Versions: `v1.2.0` for base boards, `v1.2.0-pm` for Küche —
   the same strings `flash.sh` stamps.
3. HA shows the update (the boards check every 6 h; to check right away, use
   *Check for updates* in HA or restart the board).

**Pre-releases** are ignored by the server — use them to tag without
offering an update. **Rollback**: on the server, `aqs-release-watcher v1.1.0`
republishes an older tag; HA then offers it as an update.

## Setting up the update server (once)

1. Proxmox: create a Debian/Ubuntu LXC (unprivileged, nesting on, 2 cores,
   1.5 GB RAM + 1 GB swap, 16 GB disk), hostname `aqs-firmware`, DHCP on
   the LAN bridge. Current install: CT 106 on `pve`, Ubuntu 24.04,
   192.168.178.151.
2. Inside the container, as root:
   ```bash
   apt-get update && apt-get install -y curl
   bash <(curl -fsSL https://raw.githubusercontent.com/roepkefeller/air-quality-sensor/main/tools/setup-update-server.sh)
   ```
3. From the Mac you flash from:
   ```bash
   scp firmware/secrets.yaml root@aqs-firmware.fritz.box:/etc/aqs-firmware/secrets.yaml
   ```
4. On the server: `aqs-release-watcher` (publishes the latest release now).
5. Check from the LAN: `curl http://aqs-firmware.fritz.box/kueche/manifest.json`

The boards reach the server as `http://aqs-firmware.fritz.box` (Fritzbox
names DHCP clients `<hostname>.fritz.box`). If your boards resolve DNS
elsewhere (e.g. AdGuard without forwarding to the Fritzbox), give the LXC a
fixed IP and set `update_server` in `firmware/air-quality-sensor.yaml`.

Logs: `journalctl -u aqs-release-watcher -e`. Builds take ~6 min per board
the first time (ESP-IDF), ~2 min afterwards.

Through `pct exec` the PATH lacks `/usr/local/bin`; call
`/usr/local/bin/aqs-release-watcher` there.

## Bootstrap

Boards flashed before this change don't have the update entity yet. Flash
each once by hand from a checkout of the first release that has it:

```bash
cd firmware
./flash.sh kueche
./flash.sh schlafzimmer
./flash.sh wohnzimmer
```

After that, everything goes through releases. `flash.sh` stays for
development and recovery (push OTA with the OTA password still works).

## Security notes

- Images contain the WiFi password, so they're only built on the server at
  home, never on GitHub. The GitHub workflow (`firmware-ci.yml`) only
  compiles with dummy secrets to catch errors.
- No self-hosted GitHub runner: on a public repo a stranger's pull request
  could run code on it. The server only pulls from GitHub.
- Updates are plain HTTP on the LAN; the boards check the MD5 from the
  manifest. Anyone on your LAN could in principle serve a manifest, but
  nothing installs without your click in HA.
