#!/usr/bin/env bash
# End-to-end test with the real Godot client: starts a server, runs two headless clients
# (Linux build of the same project) that register, create characters, enter the world,
# walk, chat, and must see each other; a third fights, loots, dies, and recovers its corpse; then a
# Wizard and a Cleric train at their trainers and kill with spells; a Tinkerling Magician sends its
# summoned pet in, and a Bard kills in melee while singing; two shoppers run the item economy. Usage: tools/scripts/client_e2e.sh <godot> <build-dir> [port]
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
# The Phase 1-5 runs play in the test zone (their camps, trainers and vendors are there).
EMBERFALL_START_ZONE=proving_flats EMBERFALL_DB_PATH="$W/t.db" EMBERFALL_LOG_DIR="$W/logs" EMBERFALL_PORT=$PORT EMBERFALL_HEALTH_PORT=0 \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$W/tls/server.crt" EMBERFALL_TLS_KEY="$W/tls/server.key" \
  EMBERFALL_CONTENT_PATH=content "$BUILD/server/emberfall-server" > "$W/server.out" 2>&1 &
SP=$!
sleep 1
run_client() {  # user char race class city say result gender
  timeout 90 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user "$1" \
    --pass password123 --char "$2" --race "$3" --class "$4" --city "$5" --gender "$8" --walk 6 --say "$6" \
    --expect-peers 1 --expect-models --expect-anims idle,run --reset-pins --result "$7" > "$7.log" 2>&1
}
# Both walk (the run animation), see each other, and load real models (one female, one male).
run_client e2ealice Ysolde highlander warrior emberhold "hello from A" "$W/a.json" 0 &
A=$!
run_client e2ebob Torvik deepkin shaman cragmouth "hello from B" "$W/b.json" 1 &
B=$!
# Phase 2 through the real UI code: kill and loot three rats, then die to a thornback boar,
# press Respawn, walk back, and recover the corpse with its pack.
timeout 400 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2efighter   --pass password123 --char Brawna --fight 3 --camp 50,0 --die-at -51.4,191.2 --timeout 380 --reset-pins --expect-models --expect-anims idle,run,attack1,attack2,hit,death   --result "$W/c.json" > "$W/c.json.log" 2>&1 &
C=$!
fail=0
wait $A || fail=1
wait $B || fail=1
wait $C || { echo "FAIL: combat client"; fail=1; }
# Phase 3 through the real UI: a Wizard and then a Cleric buy a scroll from their trainer,
# scribe it, buff themselves, memorize it, and kill with spells.
timeout 300 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2ewizard --pass password123 --char Pyrelle --class wizard --caster 2 --camp 40,0 --timeout 280 --reset-pins --expect-models --expect-anims idle,run,cast --result "$W/w.json" > "$W/w.json.log" 2>&1 || { echo "FAIL: wizard client"; fail=1; }
timeout 300 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2ecleric --pass password123 --char Ansel --race deepkin --class cleric --city cragmouth --caster 2 --camp 40,0 --timeout 280 --reset-pins --expect-models --expect-anims idle,run,cast --gender 1 --result "$W/cl.json" > "$W/cl.json.log" 2>&1 || { echo "FAIL: cleric client"; fail=1; }
# Phase 4 through the real UI: a pet class and a Bard (new races, the pet window and the song indicator).
timeout 300 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2emage --pass password123 --char Fizzwick --race tinkerling --class magician --city cragmouth --pet 2 --camp 40,0 --timeout 280 --reset-pins --expect-models --expect-anims idle,run,cast --result "$W/mg.json" > "$W/mg.json.log" 2>&1 || { echo "FAIL: magician client"; fail=1; }
# One at a time: two killers sharing the rat camp would each find the other's corpses first.
timeout 300 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2ebard --pass password123 --char Lirael --class bard --bard 2 --camp 40,0 --timeout 280 --reset-pins --expect-models --expect-anims idle,run,attack1 --gender 0 --result "$W/bd.json" > "$W/bd.json.log" 2>&1 || { echo "FAIL: bard client"; fail=1; }
# Phase 5 through the real UI: the starting kit, a vendor (buy, equip, sell), food, the bank (an item
# and coin, in and out) and the auction board (list, then cancel), for a plate class and a cloth class.
timeout 200 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2eshopper --pass password123 --char Hagglor --class warrior --items --timeout 180 --reset-pins --result "$W/it.json" > "$W/it.json.log" 2>&1 || { echo "FAIL: items client (warrior)"; fail=1; }
timeout 200 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2eshopwiz --pass password123 --char Tallow --class wizard --items --timeout 180 --reset-pins --result "$W/iw.json" > "$W/iw.json.log" 2>&1 || { echo "FAIL: items client (wizard)"; fail=1; }
kill -TERM $SP
wait $SP || fail=1
SP=""

# Phase 6 through the real UI: a new Emberhold citizen starts in the city, walks out of the east gate
# into the Emberglade, into the Rattlebone Warren, and back out (four zone changes' worth of worlds).
EMBERFALL_DB_PATH="$W/t3.db" EMBERFALL_LOG_DIR="" EMBERFALL_PORT=$PORT EMBERFALL_HEALTH_PORT=0 \
  EMBERFALL_LOGIN_PORT=$((PORT + 2)) EMBERFALL_TLS_CERT="$W/tls/server.crt" EMBERFALL_TLS_KEY="$W/tls/server.key" \
  EMBERFALL_CONTENT_PATH=content "$BUILD/server/emberfall-server" > "$W/server3.out" 2>&1 &
SP=$!
sleep 1
timeout 300 "$GODOT" --headless --path client -- --autotest --server "127.0.0.1:$PORT" --user e2ewalker --pass password123 --char Wendolin --class warrior --zone-walk --timeout 280 --reset-pins --result "$W/zw.json" > "$W/zw.json.log" 2>&1 || { echo "FAIL: zone walk client"; fail=1; }
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

for f in "$W/a.json" "$W/b.json" "$W/c.json" "$W/w.json" "$W/cl.json" "$W/mg.json" "$W/bd.json" "$W/it.json" "$W/iw.json" "$W/zw.json" "$W/pin.json"; do
  echo "== $f"; cat "$f" 2>/dev/null || { echo "(missing)"; fail=1; }; echo
done
grep -h "SCRIPT ERROR" "$W"/*.log && fail=1
[ $fail -eq 0 ] && echo "CLIENT E2E PASSED" || { echo "CLIENT E2E FAILED (see $W)"; tail -20 "$W"/*.log; }
exit $fail
