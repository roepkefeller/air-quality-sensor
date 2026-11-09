# Update server

Everything that runs on the firmware update server (Proxmox LXC
`aqs-firmware`, currently CT 106 / 192.168.178.151). The files here are
byte-identical to what's installed; `setup.sh` installs them and can be
re-run at any time.

- `create-lxc.sh` – create the container on the Proxmox host
- `setup.sh` – install/update everything inside the container
- `aqs-release-watcher.sh` – checks GitHub releases, builds and publishes
- `aqs-release-watcher.service` / `.timer` – runs the watcher every 10 min
- `aqs-firmware.nginx.conf` – serves `/srv/aqs-firmware` to the boards

The build itself is `../build-release.sh` (pinned ESPHome in
`../requirements-build.txt`), run from the checked-out release tag.
Not in the repo: `/etc/aqs-firmware/secrets.yaml`.

Full description, release flow and rollback: [docs/ota-server.md](../../docs/ota-server.md).
