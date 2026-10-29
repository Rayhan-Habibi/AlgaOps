# Deployment on a Raspberry Pi

Reference target: Raspberry Pi 4 or 5 (2 GB+), Raspberry Pi OS Bookworm
64-bit, SSD or high-endurance SD card, ideally a small UPS and an RTC module.
The gateway runs entirely on the plant network; it needs no internet.

> The scripts in `deploy/` were written for this target but have **not** been
> run on a physical Raspberry Pi by the project so far.  Check every step on
> your unit.

## Install

On a PC: build the dashboard (`npm run build` in `dashboard/`) and, for an
offline site, download the wheels:

```bash
deploy/raspberry-pi/build_wheelhouse.sh wheelhouse 3.11   # aarch64 wheels for Bookworm's Python
```

Copy the repository (with `dashboard/dist` and `wheelhouse/`) to the Pi, then:

```bash
sudo deploy/raspberry-pi/install.sh --wheelhouse wheelhouse   # or without the option when online
```

The script installs Mosquitto, chrony and the build tools; builds and tests the
C core; installs the gateway into `/opt/algaops/venv`; creates
`/etc/algaops/gateway.toml`, random `api_token` and `mqtt_password`; configures
Mosquitto with password authentication and the topic ACL; installs the
hardened `algaops-gateway.service`.  Re-running keeps existing configuration.

| Path | Content |
|---|---|
| `/etc/algaops/` | `gateway.toml`, `policy.default.toml`, `api_token`, `mqtt_password` (root:algaops 0640) |
| `/var/lib/algaops/` | SQLite database and its writer lock (`algaops.db.writer.lock`, never delete it while the service runs), `checkpoints.jsonl`, `exports/` |
| `/opt/algaops/` | `venv/`, `lib/libalgaops.so`, `dashboard/`, `docs/` |
| `/etc/mosquitto/conf.d/algaops.conf`, `/etc/mosquitto/algaops/` | broker settings, `passwd`, `acl.conf` |
| `/etc/chrony/conf.d/algaops.conf` | time service for the nodes (adjust `allow` subnet) |

## Sensor nodes

```bash
sudo mosquitto_passwd /etc/mosquitto/algaops/passwd esp32-a1b2c3d4e5f6   # user = node id
sudo systemctl reload mosquitto
```

In the firmware set the MQTT URI (`mqtt://<Pi address>:1883`), the same user
name and password, and the Pi as SNTP server.

## Network and access

* Keep the Pi on the plant LAN.  Never forward ports from the internet.
* Firewall (example with nftables/ufw): allow 1883/tcp and 123/udp from the
  node subnet only; SSH with keys from the maintenance network only.
* The API and dashboard listen on 127.0.0.1:8080.  Open them from a laptop with
  `ssh -L 8080:127.0.0.1:8080 pi@<address>`, or serve them on the LAN by setting
  `[api] host = "<LAN address>"` (the API token is already configured and is
  required for every write action; enter it under the dashboard's settings).
  For HTTPS put a reverse proxy with an internal certificate in front.
* Read-only plant data (optional `[pbr]` CSV) needs written approval of the PBR
  owner (`permission_confirmed = true`); it is shown as context, never used for
  diagnosis.

## Operations

```bash
systemctl status algaops-gateway
journalctl -u algaops-gateway -f
curl -s http://127.0.0.1:8080/api/health     # process up
curl -s http://127.0.0.1:8080/api/ready      # database readable, pipeline running, broker connected (503 otherwise)
sudo -u algaops /opt/algaops/venv/bin/algaops-gateway --config /etc/algaops/gateway.toml verify
```

* **Backups**: `sqlite3 /var/lib/algaops/algaops.db ".backup '/media/usb/algaops-$(date +%F).db'"`
  works while the gateway runs; copy `checkpoints.jsonl` and export bundles to
  other media as well.  The external checkpoints are what expose a later
  rewrite of the database ([evidence-verification.md](evidence-verification.md)).
* **Storage and retention**: about 4 KB per window, ~276 MB per node-day at
  1.28 s windows; plan the disk accordingly (an SSD for several nodes).  To
  bound it, add `retention_days = N` under `[storage]` in
  `/etc/algaops/gateway.toml` and restart the service: 10 minutes after the
  start and then daily, rows older than N days go into an archive bundle in
  `/var/lib/algaops/exports/` (`algaops-evidence-…-archive-….zip`) and are then
  pruned ([procedure](evidence-verification.md#retention-archive-then-prune)).
  Preview with `sudo -u algaops /opt/algaops/venv/bin/algaops-gateway --config
  /etc/algaops/gateway.toml prune --dry-run` (safe while the service runs).
  A manual `prune`, like `baseline
  import|activate`, needs the service stopped (`sudo systemctl stop
  algaops-gateway`): while the service runs it is refused with exit status 2
  and a message naming the process that holds
  `/var/lib/algaops/algaops.db.writer.lock`.  If the service is started during
  a manual `prune`, it exits with status 2, and the unit's `Restart=on-failure`
  starts it again 5 s later until the prune has finished.  Copy archives (and
  `checkpoints.jsonl`) to other media before deleting them on the Pi.  The
  audit chain is never pruned (~60 MB per
  node-day) and every archive holds all of it: each daily archive takes about
  13 MB more on disk per node-day of chain, and longer to write.  The memory
  for writing one stays flat however long the chain grows (about 10 MB on a
  desktop PC, not measured on a Pi; see the Limits there).  Pruned pages are
  reused rather than returned; `sqlite3 /var/lib/algaops/algaops.db VACUUM`
  with the service stopped shrinks the file.  After updating to a version with
  retention, the first start builds an index on `diagnostics` (once).
* **Updates**: replace the repository, run `install.sh` again (configuration,
  secrets and data stay), check `verify`.
* **Clock**: without internet chrony serves the Pi's own clock; after a power
  cut without RTC the date may be wrong (nodes and gateway still agree, so
  freshness checks hold, but timestamps in reports are off).

## Containers (alternative)

`deploy/docker/compose.yaml` runs Mosquitto and the gateway (image built from
`deploy/docker/gateway.Dockerfile`, which also runs the core's CTest).  Create
the secret files listed at the top of the compose file first.
