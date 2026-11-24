#!/usr/bin/env bash
# End-to-end smoke test (Phase 8): build the server, start it, and play one character through
# login -> create -> level a few levels -> loot -> trade -> logout with the bot client. Fails on any
# error: a bot failure, a sanitizer report, an error in the server log, or an unclean shutdown.
#
# Usage: tools/scripts/smoke_test.sh [build-preset] [port]      (default: release, 37777)
#   SMOKE_SKIP_BUILD=1  use the existing build instead of configuring and building first
set -u
PRESET=${1:-release}
PORT=${2:-37777}
HPORT=$((PORT + 1))
BUILD="build/$PRESET"
WORK=$(mktemp -d)
fail=0
step() { echo "== $*"; }

if [ "${SMOKE_SKIP_BUILD:-0}" != "1" ]; then
  step "build ($PRESET)"
  cmake --preset "$PRESET" > "$WORK/configure.log" 2>&1 \
    || { echo "FAIL: configure (see $WORK/configure.log)"; tail -30 "$WORK/configure.log"; exit 1; }
  cmake --build --preset "$PRESET" > "$WORK/build.log" 2>&1 \
    || { echo "FAIL: build (see $WORK/build.log)"; tail -30 "$WORK/build.log"; exit 1; }
fi
SERVER="$BUILD/server/emberfall-server"
BOT="$BUILD/tools/bot/emberfall-bot"

step "start the server"
# A fixed seed (Phase 12): the fighter's run no longer hangs on lucky or unlucky dice.
EMBERFALL_RNG_SEED=4242 EMBERFALL_START_ZONE=proving_flats EMBERFALL_DB_PATH="$WORK/smoke.db" EMBERFALL_LOG_DIR="$WORK/logs" EMBERFALL_PORT=$PORT \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$WORK/tls/server.crt" EMBERFALL_TLS_KEY="$WORK/tls/server.key" \
  EMBERFALL_HEALTH_PORT=$HPORT EMBERFALL_HEALTH_BIND=127.0.0.1 EMBERFALL_CONTENT_PATH=content \
  "$SERVER" > "$WORK/server.out" 2>&1 &
SP=$!
cleanup() { [ -n "$SP" ] && kill -TERM "$SP" 2>/dev/null && wait "$SP" 2>/dev/null; SP=""; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM
up=0
for _ in $(seq 100); do curl -sf --max-time 2 "http://127.0.0.1:$HPORT/health" > /dev/null && { up=1; break; }; sleep 0.1; done
[ $up -eq 1 ] || { echo "FAIL: the server did not come up"; cat "$WORK/server.out"; exit 1; }

step "login, create, fight to level 3 and loot every kill, then trade (in the background)"
"$BOT" --port "$PORT" --user smoker --name Smokey --fight 6 --until-level 3 --trade-with Partner --duration 600 --content content \
  > "$WORK/fighter.out" 2>&1 &
FP=$!
# The partner logs in once the fighter is heading back to trade.
for _ in $(seq 1200); do
  grep -q "trade: offering" "$WORK/fighter.out" && break
  kill -0 $FP 2>/dev/null || break
  sleep 0.5
done
step "partner: login, create, accept the trade, logout"
"$BOT" --port "$PORT" --user partner --name Partner --trade-accept Smokey --content content > "$WORK/partner.out" 2>&1 \
  || { echo "FAIL: trade partner"; fail=1; }
wait $FP || { echo "FAIL: fighter"; fail=1; }
grep -E "created|in world|reached level|fight:|trade|PASS|FAIL" "$WORK/fighter.out" "$WORK/partner.out" | sed 's|.*/||'

step "log back in: the character kept its level"
"$BOT" --port "$PORT" --user smoker --name Smokey --duration 1 --expect-level 3 > "$WORK/relogin.out" 2>&1 \
  || { echo "FAIL: re-login"; grep -E "FAIL" "$WORK/relogin.out"; fail=1; }
grep -qE "account exists" "$WORK/relogin.out" || { echo "FAIL: the account was not found again"; fail=1; }

step "stop the server"
kill -TERM $SP
wait $SP
rc=$?
SP=""
[ $rc -eq 0 ] || { echo "FAIL: server exit code $rc"; fail=1; }
grep -q "shutdown complete" "$WORK/logs/emberfall-server.log" || { echo "FAIL: no graceful shutdown"; fail=1; }
if grep -qE "AddressSanitizer|runtime error:" "$WORK/server.out"; then echo "FAIL: sanitizer report"; grep -E "AddressSanitizer|runtime error:" "$WORK/server.out" | head; fail=1; fi
if grep -E "\[(error|critical)\]" "$WORK/logs/emberfall-server.log"; then echo "FAIL: errors in the server log"; fail=1; fi
[ $fail -eq 0 ] && echo "SMOKE TEST PASSED" || echo "SMOKE TEST FAILED (logs in $WORK)"
[ $fail -eq 0 ] && rm -rf "$WORK"
exit $fail
