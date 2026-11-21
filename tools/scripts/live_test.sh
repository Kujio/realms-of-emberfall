#!/usr/bin/env bash
# Live end-to-end test: starts a real server, runs bots against it over UDP, checks health,
# then stops the server with SIGTERM and verifies a graceful shutdown.
# Usage: tools/scripts/live_test.sh <build-dir> [port]
set -u
BUILD=${1:-build/debug}
PORT=${2:-17777}
HPORT=$((PORT + 1))
WORK=$(mktemp -d)
SERVER="$BUILD/server/emberfall-server"
BOT="$BUILD/tools/bot/emberfall-bot"
fail=0

EMBERFALL_DB_PATH="$WORK/test.db" EMBERFALL_LOG_DIR="$WORK/logs" EMBERFALL_PORT=$PORT \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$WORK/tls/server.crt" EMBERFALL_TLS_KEY="$WORK/tls/server.key" \
  EMBERFALL_HEALTH_PORT=$HPORT EMBERFALL_HEALTH_BIND=127.0.0.1 EMBERFALL_CONTENT_PATH=content \
  "$SERVER" > "$WORK/server.out" 2>&1 &
SP=$!
cleanup() { [ -n "$SP" ] && kill -TERM "$SP" 2>/dev/null && wait "$SP" 2>/dev/null; SP=""; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM
for _ in $(seq 50); do curl -sf "http://127.0.0.1:$HPORT/health" > /dev/null && break; sleep 0.1; done

"$BOT" --port "$PORT" --user alice --name Aldric --duration 6 --expect-peers 1 > "$WORK/a.out" 2>&1 &
A=$!
"$BOT" --port "$PORT" --user bobby --name Brenna --race deepkin --class cleric --city cragmouth --duration 6 --expect-peers 1 > "$WORK/b.out" 2>&1 &
B=$!
wait $A || { echo "FAIL: bot A"; fail=1; }
wait $B || { echo "FAIL: bot B"; fail=1; }
grep -E "walked|saw|PASS|FAIL" "$WORK/a.out" "$WORK/b.out"

# Second login reuses the saved account/character (exercises the login path and persistence).
"$BOT" --port "$PORT" --user alice --name Aldric --duration 1 --delete-after > "$WORK/a2.out" 2>&1 || { echo "FAIL: re-login"; fail=1; }
grep -E "account exists|in world|delete|PASS|FAIL" "$WORK/a2.out"

# Phase 2: a new Warrior kills level-appropriate mobs, loots, and levels up; another dies to a
# far stronger mob, respawns at its bind point, walks back, and recovers its corpse.
# They run one after the other: loot goes to whoever did the most damage, so two bots racing for
# the same fresh rat would (correctly) lose each other's loot.
"$BOT" --port "$PORT" --user fighter --name Fightor --fight 6 --duration 240 --content content > "$WORK/f.out" 2>&1   || { echo "FAIL: fight bot"; fail=1; }
"$BOT" --port "$PORT" --user doomed --name Doomed --die --fight 3 --duration 240 --content content > "$WORK/d.out" 2>&1   || { echo "FAIL: death bot"; fail=1; }
grep -E "reached level|slain|died carrying|respawned|recovered|fight:|PASS|FAIL" "$WORK/f.out" "$WORK/d.out"

"$BOT" --port "$PORT" --user vers --protocol-version 99 --expect-version-reject || { echo "FAIL: version reject"; fail=1; }
"$BOT" --port "$PORT" --user fuzzer --fuzz 3000 || { echo "FAIL: fuzz"; fail=1; }

echo "health: $(curl -s "http://127.0.0.1:$HPORT/health")"
kill -TERM $SP
wait $SP
rc=$?
SP=""
echo "server exit code: $rc"
[ $rc -eq 0 ] || { echo "FAIL: server did not exit cleanly"; fail=1; }
grep -q "shutdown complete" "$WORK/logs/emberfall-server.log" || { echo "FAIL: no graceful shutdown"; fail=1; }
if grep -qE "AddressSanitizer|runtime error:" "$WORK/server.out"; then echo "FAIL: sanitizer report"; grep -E "AddressSanitizer|runtime error:" "$WORK/server.out" | head; fail=1; fi
echo "server warnings: $(grep -c '\[warning\]' "$WORK/logs/emberfall-server.log") (violations from the fuzzer are expected)"
grep -E "\[(info|error|critical)\]" "$WORK/logs/emberfall-server.log" | grep -vE "kicking|disconnected|rejected" | tail -15
[ $fail -eq 0 ] && echo "LIVE TEST PASSED" || echo "LIVE TEST FAILED (logs in $WORK)"
[ $fail -eq 0 ] && rm -rf "$WORK"
exit $fail
