# Deploying a Realms of Emberfall server

This walks through a fresh **Ubuntu 24.04 VPS** (22.04 works the same) from nothing to a server
players can connect to, then covers backups, restores, updates and monitoring. Two paths:

- **Path A, Docker** (recommended): one container, the database in a Docker volume.
- **Path B, systemd**: build from source and run under systemd.

Sizing: one core and 512 MB of RAM run a few hundred players (measured below). The server is a single
process; it uses one core for the game and short bursts on other threads for logins and saving.

## Ports and firewall

| port | protocol | what | open to |
|---|---|---|---|
| 7777 | UDP | game traffic (ENet) | everyone |
| 7779 | TCP | TLS login service (passwords only ever travel here) | everyone |
| 7778 | TCP | health/admin endpoint (`GET /health`) | **localhost only** |

```bash
sudo ufw allow OpenSSH
sudo ufw allow 7777/udp
sudo ufw allow 7779/tcp
sudo ufw enable
```

Your hosting provider may have its own firewall (security groups); open the same two ports there.
Never expose 7778 publicly: it shows server internals. Docker publishes it on 127.0.0.1 only.

## Path A: Docker

```bash
# 1. Docker Engine and the compose plugin
sudo apt-get update
sudo apt-get install -y ca-certificates curl git
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker "$USER"   # log out and back in afterwards

# 2. The code (or unpack a release's source archive), owned by you so deploy/.env can be written
sudo install -d -o "$USER" -g "$USER" /opt/emberfall
git clone <your-repository-url> /opt/emberfall
cd /opt/emberfall

# 3. Optional settings (all EMBERFALL_* keys from the table below work)
cat > deploy/.env <<'ENV'
EMBERFALL_MAX_PLAYERS=200
EMBERFALL_MOTD=Welcome to our realm! Events every Saturday.
ENV

# 4. Build and start (the build stage runs the unit tests)
docker compose -f deploy/docker-compose.yml --env-file deploy/.env up -d --build
docker compose -f deploy/docker-compose.yml logs -f      # Ctrl+C to stop following
```

A release also publishes a ready-made image, `ghcr.io/<owner>/<repo>/emberfall-server:<version>`; to
use it, put that name in `image:` in `deploy/docker-compose.yml` and remove the `build:` block.

The data volume `emberfall-data` (mounted at `/var/lib/emberfall`) holds the database, the TLS key
and certificate, the logs and the backups. Removing the container never removes the volume.

**Publish the fingerprint.** On first start the server makes its TLS certificate and logs its SHA-256
fingerprint. Players see that fingerprint the first time they connect and should compare it with one
you published (website, Discord):

```bash
docker compose -f deploy/docker-compose.yml logs | grep fingerprint
curl -s http://127.0.0.1:7778/health | grep -o '"tls_fingerprint":"[^"]*"'
```

**Game masters.** Register an account in the client, then:

```bash
docker exec emberfall-server emberfall-server --grant-admin youraccount
```

(it takes effect at that account's next login; see [ADMIN.md](ADMIN.md)).

## Path B: build from source and run under systemd

```bash
# 1. Build tools (GCC 12+ or Clang 15+, CMake 3.22+)
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config libsodium-dev libssl-dev git curl

# 2. Build and test as yourself, in a checkout you own (dependencies are fetched by CMake)
git clone <your-repository-url> ~/emberfall-src
cd ~/emberfall-src
cmake --preset release && cmake --build --preset release && ctest --preset release

# 3. Install
sudo useradd --system --home-dir /var/lib/emberfall --create-home --shell /usr/sbin/nologin emberfall
sudo install -d /opt/emberfall/bin /etc/emberfall
sudo install -m 755 build/release/server/emberfall-server /opt/emberfall/bin/
sudo cp -r content /opt/emberfall/content
sudo cp -r deploy /opt/emberfall/deploy
sudo cp deploy/config/server.json /etc/emberfall/server.json
# The file's paths (/opt/emberfall/content, /var/lib/emberfall/...) already match this layout; only
# keep the health endpoint local (Docker does that with its port mapping instead):
sudo sed -i 's|"health_bind": "0.0.0.0"|"health_bind": "127.0.0.1"|' /etc/emberfall/server.json

# 4. Run it
sudo cp deploy/systemd/emberfall.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now emberfall
journalctl -u emberfall -f
```

Secrets and per-host overrides go in `/etc/emberfall/emberfall.env` (one `EMBERFALL_KEY=value` per
line; read by the unit). Grant game master rights with
`sudo -u emberfall /opt/emberfall/bin/emberfall-server --config /etc/emberfall/server.json --grant-admin NAME`.

## Configuration

`/etc/emberfall/server.json` (Docker: `deploy/config/server.json`, baked into the image) sets
defaults; any key can be overridden with an `EMBERFALL_*` environment variable. Unknown keys fail the
start with a clear message. The useful ones:

| key / variable | default | |
|---|---|---|
| `port` / `EMBERFALL_PORT` | 7777 | game UDP port |
| `login_port` / `EMBERFALL_LOGIN_PORT` | 7779 | TLS login TCP port |
| `max_players` / `EMBERFALL_MAX_PLAYERS` | 200 | in the world at once (measured capacity below) |
| `motd` / `EMBERFALL_MOTD` | a welcome line | shown on every player's login screen |
| `db_path` / `EMBERFALL_DB_PATH` | data/emberfall.db | the SQLite database |
| `db_check` / `EMBERFALL_DB_CHECK` | quick | integrity check at start: quick, full or off |
| `autosave_seconds` | 60 | everyone is also saved on logout, zone change and shutdown |
| `linger_seconds` / `EMBERFALL_LINGER_SECONDS` | 30 | a character dropped mid-fight stays this long |
| `login_timeout_seconds` | 120 | connections that never log in are dropped |
| `login_attempts_per_minute`, `login_attempt_burst` | 12, 10 | per IP, at the TLS login service |
| `max_connections_per_ip` | 8 | game connections per IP |
| `log_level` / `EMBERFALL_LOG_LEVEL` | info | trace, debug, info, warn, error |
| `tick_rate` | 20 | simulation steps per second |

## Logs

The server writes `logs/emberfall-server.log` (10 MB per file, 5 files kept) and `logs/audit.log` (game
master actions, 10 MB x 20) under its data directory, rotating them itself; nothing else is needed.
Standard output goes to `docker logs` or the journal. `deploy/docker-compose.yml` caps the container's
log at 5 files of 10 MB (Docker itself keeps logs forever unless told otherwise); for systemd, cap the
journal with `SystemMaxUse=500M` in `/etc/systemd/journald.conf`.

## Backups

`deploy/backup.sh` takes an online, integrity-checked copy while the server runs and prunes copies
older than 14 days:

```bash
sudo crontab -e
# every 6 hours (Docker; use "systemd" for Path B):
0 */6 * * * /opt/emberfall/deploy/backup.sh docker >> /var/log/emberfall-backup.log 2>&1
```

Copies land in `/var/lib/emberfall/backups/` (inside the data volume for Docker). Also keep a copy of
`/var/lib/emberfall/tls/` (once is enough): with the same key, players' pinned fingerprints stay valid
after a rebuild. Copy them off the
machine too (for example `rclone` or `restic` to object storage): a backup on the same disk does not
survive the disk. To pull one out of the Docker volume:

```bash
docker cp emberfall-server:/var/lib/emberfall/backups/emberfall-20261122-060000.db ./
```

## Restoring from a backup

The server checks the database at every start (`db_check`). If the file is damaged it logs
`failed its integrity check`, refuses to start (exit code 3; systemd does not restart it) and leaves
the file untouched. To restore:

```bash
# Docker
docker compose -f deploy/docker-compose.yml stop
docker run --rm -v emberfall-data:/data -v "$PWD":/backup ubuntu:24.04 bash -c '
  cd /data && mkdir -p damaged && mv emberfall.db* damaged/ 2>/dev/null;
  cp /backup/emberfall-20261122-060000.db emberfall.db && chown 10001:10001 emberfall.db'
docker compose -f deploy/docker-compose.yml run --rm emberfall --check-db   # "integrity check passed"
docker compose -f deploy/docker-compose.yml up -d

# systemd
sudo systemctl stop emberfall
cd /var/lib/emberfall && sudo mkdir -p damaged && sudo mv emberfall.db* damaged/
sudo cp backups/emberfall-20261122-060000.db emberfall.db && sudo chown emberfall: emberfall.db
sudo -u emberfall /opt/emberfall/bin/emberfall-server --config /etc/emberfall/server.json --check-db
sudo systemctl start emberfall
```

Move the old `emberfall.db-wal` and `emberfall.db-shm` aside with the damaged file (as above): they
belong to it, not to the backup. Players lose what happened after the backup was taken.

## Updating without losing data

1. Take a backup (`deploy/backup.sh docker`).
2. Get the new version (`git pull`, or a new release image tag).
3. Rebuild and restart:
   - Docker: `docker compose -f deploy/docker-compose.yml up -d --build`
   - systemd: build as in Path B step 2, copy the binary and `content/`, then `sudo systemctl restart emberfall`.

A restart sends every player a "server is shutting down; your character has been saved" message,
saves everyone and exits within a couple of seconds. The database schema migrates itself forward on the
first start of the new version (it is never migrated backwards: keep the backup until you are happy).
A new version that changes the network protocol tells old clients to update when they connect; publish
the new client at the same time.

## Monitoring

```bash
curl -s http://127.0.0.1:7778/health | python3 -m json.tool
```

`players_online`, `tick_ms_avg`/`tick_ms_p99` (the budget is 50 ms at 20 Hz), `cpu_pct`, `rss_kib`,
`send_bytes_per_s_per_player`, `db_queue` (saves waiting; should stay near 0), `lingering`, and
`tick_phase_ms` (where the tick goes). Docker's health check uses this endpoint; point an uptime monitor
or Prometheus' JSON exporter at it through an SSH tunnel if you want graphs.

## Measured capacity

The Phase 8 load test (`tools/scripts/load_test.sh`): bots log in over TLS, then walk, fight and chat
in four zones of the test band. On one core of a desktop-class Linux machine, release build:

| players | per zone | tick mean | tick p99 | CPU (one core) | memory | sent per player |
|---|---|---|---|---|---|---|
| 150 | ~38 | 3.5 ms | 5.5 ms | 12% | 101 MB | 10 KB/s |
| 300 | ~75 | 8.1 ms | 11.5 ms | 22% | 102 MB | 17.5 KB/s |

The cost grows with players *per zone* (everyone in view of everyone): spread out, a few hundred
players fit easily within the 50 ms budget. Upstream bandwidth is the first limit on a small VPS:
300 players crowded together send about 5 MB/s; budget 10-20 KB/s per player.

## Troubleshooting

- **Players cannot connect:** check the provider firewall and `ufw status` for 7777/udp and 7779/tcp;
  `docker compose ps` should show the container healthy.
- **"Please update your client":** the client and server versions differ; give players the client
  from the same release.
- **"This server's identity has changed":** the TLS key was regenerated (a new data volume). Restore
  `tls/server.key` and `tls/server.crt` from a backup of the data directory, or tell players to remove
  the old pin as the message explains.
- **Server exits with code 3:** the database is damaged; see "Restoring from a backup".
