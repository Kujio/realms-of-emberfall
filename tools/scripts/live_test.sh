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

# The bots play in the test zone (EMBERFALL_START_ZONE): their camps and trainers are there.
EMBERFALL_START_ZONE=proving_flats EMBERFALL_DB_PATH="$WORK/test.db" EMBERFALL_LOG_DIR="$WORK/logs" EMBERFALL_PORT=$PORT \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$WORK/tls/server.crt" EMBERFALL_TLS_KEY="$WORK/tls/server.key" \
  EMBERFALL_HEALTH_PORT=$HPORT EMBERFALL_HEALTH_BIND=127.0.0.1 EMBERFALL_CONTENT_PATH=content \
  "$SERVER" > "$WORK/server.out" 2>&1 &
SP=$!
cleanup() { [ -n "$SP" ] && kill -TERM "$SP" 2>/dev/null && wait "$SP" 2>/dev/null; SP=""; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM
# Up to 30 s: an ASan build loads the content slowly (over 5 s on some machines).
for _ in $(seq 300); do curl -sf "http://127.0.0.1:$HPORT/health" > /dev/null && break; sleep 0.1; done

"$BOT" --port "$PORT" --user alice --name Aldric --duration 6 --expect-peers 1 > "$WORK/a.out" 2>&1 &
A=$!
"$BOT" --port "$PORT" --user bobby --name Brenna --race deepkin --class cleric --city cragmouth --gender 1 --duration 6 --expect-peers 1 > "$WORK/b.out" 2>&1 &
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

# Phase 3: a Wizard and a Cleric train at their trainers (buy, scribe, buff, memorize) and kill
# with spells. One after the other, for the same reason as the fighters.
"$BOT" --port "$PORT" --user wizbot --name Wizbot --class wizard --caster --fight 3 --duration 240 --content content > "$WORK/w.out" 2>&1   || { echo "FAIL: wizard bot"; fail=1; }
"$BOT" --port "$PORT" --user clerbot --name Clerbot --race deepkin --class cleric --city cragmouth --caster --fight 3 --duration 240 --content content > "$WORK/c.out" 2>&1   || { echo "FAIL: cleric bot"; fail=1; }
grep -E "training:|spells:|fight:|PASS|FAIL" "$WORK/w.out" "$WORK/c.out"

# Phase 14: two groups of two form a raid by slash commands. Each must see the raid of four and hear
# the leader on the raid channel. Each invitation is sent twice, a few seconds apart, in case its target
# entered the world late (a repeat that is no longer needed is refused harmlessly).
"$BOT" --port "$PORT" --user raidlead --name Raidus --duration 22 --accept-invites --expect-raid 4 \
  --commands "3:invite Rinna|7:invite Rinna|10:raidinvite Rollo|14:raidinvite Rollo|17:rsay raid check" > "$WORK/r1.out" 2>&1 &
R1=$!
"$BOT" --port "$PORT" --user raidtwo --name Rinna --class cleric --duration 22 --accept-invites --expect-raid 4 --expect-raid-chat "raid check" > "$WORK/r2.out" 2>&1 &
R2=$!
"$BOT" --port "$PORT" --user raidthree --name Rollo --duration 22 --accept-invites --expect-raid 4 --expect-raid-chat "raid check" \
  --commands "3:invite Rosa|7:invite Rosa" > "$WORK/r3.out" 2>&1 &
R3=$!
"$BOT" --port "$PORT" --user raidfour --name Rosa --class wizard --duration 22 --accept-invites --expect-raid 4 --expect-raid-chat "raid check" > "$WORK/r4.out" 2>&1 &
R4=$!
for p in $R1 $R2 $R3 $R4; do wait $p || { echo "FAIL: raid bot"; fail=1; }; done
grep -E "raid:|command:|PASS|FAIL" "$WORK"/r[1-4].out

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
