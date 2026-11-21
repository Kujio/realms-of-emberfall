#!/usr/bin/env bash
# End-to-end test with the real Godot client: starts a server, runs two headless clients
# (Linux build of the same project) that register, create characters, enter the world,
# walk, chat, and must see each other; a third fights, loots, dies, and recovers its corpse. Usage: tools/scripts/client_e2e.sh <godot> <build-dir> [port]
set -u
GODOT=$1
BUILD=${2:-build/release}
PORT=${3:-17877}
W=$(mktemp -d)
SP=""
# Never leave a server (and its ports) behind if the run is interrupted or fails midway.
cleanup() { [ -n "$SP" ] && kill -TERM "$SP" 2>/dev/null && wait "$SP" 2>/dev/null; SP=""; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM
EMBERFALL_DB_PATH="$W/t.db" EMBERFALL_LOG_DIR="$W/logs" EMBERFALL_PORT=$PORT EMBERFALL_HEALTH_PORT=0 \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$W/tls/server.crt" EMBERFALL_TLS_KEY="$W/tls/server.key" \
  EMBERFALL_CONTENT_PATH=content "$BUILD/server/emberfall-server" > "$W/server.out" 2>&1 &
SP=$!
sleep 1
run_client() {  # user char race class city say result
  timeout 90 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user "$1" \
    --pass password123 --char "$2" --race "$3" --class "$4" --city "$5" --walk 6 --say "$6" \
    --expect-peers 1 --reset-pins --result "$7" > "$7.log" 2>&1
}
run_client e2ealice Ysolde highlander warrior emberhold "hello from A" "$W/a.json" &
A=$!
run_client e2ebob Torvik deepkin shaman cragmouth "hello from B" "$W/b.json" &
B=$!
# Phase 2 through the real UI code: kill and loot three rats, then die to a thornback boar,
# press Respawn, walk back, and recover the corpse with its pack.
timeout 400 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2efighter   --pass password123 --char Brawna --fight 3 --camp 50,0 --die-at -51.4,191.2 --timeout 380 --reset-pins   --result "$W/c.json" > "$W/c.json.log" 2>&1 &
C=$!
fail=0
wait $A || fail=1
wait $B || fail=1
wait $C || { echo "FAIL: combat client"; fail=1; }
kill -TERM $SP
wait $SP || fail=1
SP=""

# Same address, new certificate: a client that pinned the old one must refuse to send its password.
EMBERFALL_DB_PATH="$W/t2.db" EMBERFALL_LOG_DIR="" EMBERFALL_PORT=$PORT EMBERFALL_HEALTH_PORT=0 \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$W/tls2/server.crt" EMBERFALL_TLS_KEY="$W/tls2/server.key" \
  EMBERFALL_CONTENT_PATH=content "$BUILD/server/emberfall-server" > "$W/server2.out" 2>&1 &
SP=$!
sleep 1
timeout 60 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2ealice \
  --pass password123 --char Ysolde --expect-error "identity has changed" --result "$W/pin.json" > "$W/pin.json.log" 2>&1 \
  || { echo "FAIL: client did not refuse a changed server certificate"; fail=1; }
# "TLS login: " (with the colon) is only logged for actual login attempts.
grep -q "TLS login: " "$W/server2.out" && { echo "FAIL: password reached the impostor server"; fail=1; }
kill -TERM $SP
wait $SP || fail=1
SP=""

for f in "$W/a.json" "$W/b.json" "$W/c.json" "$W/pin.json"; do
  echo "== $f"; cat "$f" 2>/dev/null || { echo "(missing)"; fail=1; }; echo
done
grep -h "SCRIPT ERROR" "$W"/*.log && fail=1
[ $fail -eq 0 ] && echo "CLIENT E2E PASSED" || { echo "CLIENT E2E FAILED (see $W)"; tail -20 "$W"/*.log; }
exit $fail
