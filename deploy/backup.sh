#!/usr/bin/env bash
# Online backup of the Emberfall database: a consistent, integrity-checked copy taken while the server
# runs (SQLite's backup API, via `emberfall-server --backup`), then old copies are pruned.
#
#   deploy/backup.sh docker  [backup-dir] [keep-days]   # the Docker deployment (container emberfall-server)
#   deploy/backup.sh systemd [backup-dir] [keep-days]   # a source install (/opt/emberfall, /etc/emberfall)
#
# Defaults: backups in /var/lib/emberfall/backups (inside the data volume for Docker), kept 14 days.
# Cron (as root), every 6 hours:
#   0 */6 * * * /opt/emberfall/deploy/backup.sh docker >> /var/log/emberfall-backup.log 2>&1
# Restoring: docs/DEPLOY.md, "Restoring from a backup".
set -euo pipefail
MODE=${1:-docker}
DIR=${2:-/var/lib/emberfall/backups}
KEEP_DAYS=${3:-14}
STAMP=$(date -u +%Y%m%d-%H%M%S)
NAME="emberfall-$STAMP.db"

case "$MODE" in
  docker)
    CONTAINER=${EMBERFALL_CONTAINER:-emberfall-server}
    docker exec "$CONTAINER" mkdir -p "$DIR"
    docker exec "$CONTAINER" emberfall-server --backup "$DIR/$NAME"
    docker exec "$CONTAINER" find "$DIR" -name 'emberfall-*.db' -mtime "+$KEEP_DAYS" -delete
    echo "$(date -u +%FT%TZ) backup $DIR/$NAME (inside $CONTAINER; data volume emberfall-data)"
    ;;
  systemd)
    BIN=${EMBERFALL_BIN:-/opt/emberfall/bin/emberfall-server}
    CONFIG=${EMBERFALL_CONFIG:-/etc/emberfall/server.json}
    install -d -o emberfall -g emberfall "$DIR"
    sudo -u emberfall "$BIN" --config "$CONFIG" --backup "$DIR/$NAME"
    find "$DIR" -name 'emberfall-*.db' -mtime "+$KEEP_DAYS" -delete
    echo "$(date -u +%FT%TZ) backup $DIR/$NAME"
    ;;
  *)
    echo "usage: $0 docker|systemd [backup-dir] [keep-days]" >&2
    exit 2
    ;;
esac
