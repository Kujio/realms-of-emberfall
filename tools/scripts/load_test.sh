#!/usr/bin/env bash
# Load test: starts a release server spread over several zones, runs a swarm of bots against it, and
# samples /health once a second. Reports tick time (mean and p99), CPU, memory and bandwidth per player,
# and fails if a bot could not play or the tick was unhealthy (p99 above the budget limit).
#
# Usage: tools/scripts/load_test.sh <build-dir> [bots] [seconds] [port]
#   LOAD_ZONES   comma-separated start zones (default: the test plain and three level 1-15 zones)
#   P99_LIMIT_MS fail when the worst one-minute p99 tick is above this (default 25 ms: half the 50 ms budget)
#   RAMP_MS      delay between bot logins (default 150)
set -u
BUILD=${1:-build/release}
BOTS=${2:-150}
SECONDS_IN_WORLD=${3:-120}
PORT=${4:-27777}
HPORT=$((PORT + 1))
ZONES=${LOAD_ZONES:-proving_flats,emberglade,briarwatch_wood,cragfoot_hills}
P99_LIMIT_MS=${P99_LIMIT_MS:-25}
RAMP_MS=${RAMP_MS:-150}
python3 -c 'import math, sys; v = float(sys.argv[1]); sys.exit(0 if math.isfinite(v) and v > 0 else 1)' "$P99_LIMIT_MS" 2>/dev/null \
  || { echo "P99_LIMIT_MS must be a positive number of milliseconds (got '$P99_LIMIT_MS')"; exit 2; }
WORK=$(mktemp -d)
SERVER="$BUILD/server/emberfall-server"
BOT="$BUILD/tools/bot/emberfall-bot"
fail=0

EMBERFALL_START_ZONE=$ZONES EMBERFALL_DB_PATH="$WORK/load.db" EMBERFALL_LOG_DIR="$WORK/logs" EMBERFALL_PORT=$PORT \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$WORK/tls/server.crt" EMBERFALL_TLS_KEY="$WORK/tls/server.key" \
  EMBERFALL_HEALTH_PORT=$HPORT EMBERFALL_HEALTH_BIND=127.0.0.1 EMBERFALL_CONTENT_PATH=content \
  EMBERFALL_MAX_PLAYERS=$((BOTS + 50)) EMBERFALL_MAX_CONNECTIONS_PER_IP=$((BOTS + 50)) EMBERFALL_LOG_LEVEL=warn \
  EMBERFALL_LOGIN_ATTEMPTS_PER_MINUTE=100000 EMBERFALL_LOGIN_ATTEMPT_BURST=$((BOTS + 50)) \
  "$SERVER" > "$WORK/server.out" 2>&1 &
SP=$!
cleanup() { [ -n "$SP" ] && kill -TERM "$SP" 2>/dev/null && wait "$SP" 2>/dev/null; SP=""; [ -n "${MON:-}" ] && kill "$MON" 2>/dev/null; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM
for _ in $(seq 100); do curl -sf --max-time 2 "http://127.0.0.1:$HPORT/health" > /dev/null && break; sleep 0.1; done

# One /health sample per second while the swarm plays.
(
  while kill -0 "$SP" 2>/dev/null; do
    curl -sf --max-time 2 "http://127.0.0.1:$HPORT/health" >> "$WORK/health.jsonl" && echo >> "$WORK/health.jsonl"
    sleep 1
  done
) &
MON=$!

echo "load test: $BOTS bots for ${SECONDS_IN_WORLD}s in $ZONES (ramp ${RAMP_MS} ms)"
"$BOT" --port "$PORT" --user loadbot --swarm "$BOTS" --ramp-ms "$RAMP_MS" --duration "$SECONDS_IN_WORLD" > "$WORK/swarm.out" 2>&1 || { echo "FAIL: swarm"; fail=1; }
grep "^\[swarm\]" "$WORK/swarm.out" | head -40
kill "$MON" 2>/dev/null
MON=""

python3 - "$WORK/health.jsonl" "$BOTS" "$P99_LIMIT_MS" <<'PY' || fail=1
import json, sys
path, bots, limit = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
rows = [json.loads(l) for l in open(path) if l.strip()]
full = [r for r in rows if r["players_online"] >= bots * 0.95]
if not full:
    print("FAIL: the server never had %d players online at once (peak %d)" % (bots, max((r["players_online"] for r in rows), default=0)))
    sys.exit(1)
def mean(v): return sum(v) / len(v)
avg = mean([r["tick_ms_avg"] for r in full])
p99 = max(r["tick_ms_p99"] for r in full)
worst = max(r["tick_ms_max"] for r in full)
cpu = mean([r["cpu_pct"] for r in full])
rss = max(r["rss_kib"] for r in rows) / 1024.0
out = mean([r["send_bytes_per_s"] for r in full])
inn = mean([r["receive_bytes_per_s"] for r in full])
players = mean([r["players_online"] for r in full])
zones = max(r["zones_loaded"] for r in full)
print("with %.0f players online (%d samples, %d zones loaded):" % (players, len(full), zones))
print("  tick   mean %.2f ms, p99 %.2f ms, worst %.2f ms (budget %.0f ms)" % (avg, p99, worst, full[0]["tick_budget_ms"]))
print("  cpu    %.1f%% of one core" % cpu)
print("  memory %.0f MiB resident (peak)" % rss)
print("  net    out %.0f KiB/s (%.0f B/s per player), in %.0f KiB/s (%.0f B/s per player)" % (out / 1024, out / players, inn / 1024, inn / players))
phases = {k: mean([r["tick_phase_ms"][k] for r in full]) for k in full[0].get("tick_phase_ms", {})}
if phases:
    print("  phases " + ", ".join("%s %.2f ms" % (k, v) for k, v in sorted(phases.items(), key=lambda kv: -kv[1])))
if p99 > limit:
    print("FAIL: p99 tick %.2f ms is above the %.0f ms limit" % (p99, limit))
    sys.exit(1)
PY

kill -TERM $SP
wait $SP
rc=$?
SP=""
[ $rc -eq 0 ] || { echo "FAIL: server did not exit cleanly ($rc)"; fail=1; }
if grep -qE "AddressSanitizer|runtime error:" "$WORK/server.out"; then echo "FAIL: sanitizer report"; fail=1; fi
grep -E "\[(error|critical)\]" "$WORK/logs/emberfall-server.log" | head -10
[ $fail -eq 0 ] && echo "LOAD TEST PASSED" || echo "LOAD TEST FAILED (logs in $WORK)"
[ $fail -eq 0 ] && rm -rf "$WORK"
exit $fail
