# Building and running Realms of Emberfall

## Layout

| Path | What |
|---|---|
| `server/` | C++20 game server (`emberfall-server`) and its Catch2 tests |
| `tools/bot/` | Headless C++ test client (`emberfall-bot`) |
| `protocol/messages.json` | The single wire-protocol definition |
| `tools/gen_protocol.py` | Generates `server/src/net/protocol.gen.hpp` and `client/scripts/net/protocol.gd` |
| `tools/validate_content.py` | Validates `/content` JSON (CI runs it) |
| `content/` | Game data: rules, races, classes, cities, zones and spawns, skills, abilities, items, loot tables, mobs |
| `client/` | Godot 4.7 project (GDScript, static typing) |
| `deploy/` | Dockerfile, compose file, container config |

## Protocol changes

Edit `protocol/messages.json`, bump `version` for any breaking change, then:

```sh
python tools/gen_protocol.py          # regenerate C++ and GDScript
python tools/gen_protocol.py --check  # what CI runs; fails if generated files are stale
```

Commit the generated files together with the definition.

## Server (Linux)

Requirements: CMake 3.22+, Ninja, GCC 12+ or Clang 15+, pkg-config, libsodium, and OpenSSL 3
(`apt install ninja-build pkg-config libsodium-dev libssl-dev`). All other dependencies (ENet,
nlohmann/json, spdlog, SQLite, Catch2) are fetched by CMake at configure time, pinned by version and SHA-256.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan    # ASan + UBSan
cmake --preset debug   && cmake --build --preset debug
```

Binaries land in `build/<preset>/server/emberfall-server` and `build/<preset>/tools/bot/emberfall-bot`.
Warnings are errors in every preset.

### Running

From the repository root:

```sh
./build/release/server/emberfall-server
```

With no `--config` the built-in defaults are used (game on UDP 7777, TLS login on TCP 7779,
health on TCP 7778, `data/emberfall.db`, `data/tls/`, `content/`, `logs/`, all relative to the
working directory).
`deploy/config/server.json` is the container's config (it points at `/opt/emberfall` and
`/var/lib/emberfall`), so don't use it for a native run. Every setting can be overridden with an environment variable:

| Variable | Default |
|---|---|
| `EMBERFALL_CONFIG` | (none) path to a JSON config file |
| `EMBERFALL_BIND` / `EMBERFALL_PORT` | `0.0.0.0` / `7777` |
| `EMBERFALL_LOGIN_BIND` / `EMBERFALL_LOGIN_PORT` | `0.0.0.0` / `7779` (TLS login service, TCP) |
| `EMBERFALL_TLS_CERT` / `EMBERFALL_TLS_KEY` | `data/tls/server.crt` / `data/tls/server.key` |
| `EMBERFALL_HEALTH_BIND` / `EMBERFALL_HEALTH_PORT` | `0.0.0.0` / `7778` (`0` disables) |
| `EMBERFALL_DB_PATH` | `data/emberfall.db` |
| `EMBERFALL_CONTENT_PATH` | `content` |
| `EMBERFALL_MAX_PLAYERS` | `200` |
| `EMBERFALL_TICK_RATE` | `20` |
| `EMBERFALL_AUTOSAVE_SECONDS` | `60` |
| `EMBERFALL_LOG_DIR` / `EMBERFALL_LOG_LEVEL` | `logs` / `info` (empty dir = console only) |
| `EMBERFALL_ADMIN_PASSWORD` | (empty) reserved for GM tools |

Unknown config keys, malformed values, and invalid content all stop the server at startup with a
message listing every problem. `SIGTERM`/`SIGINT` save every character and exit cleanly.

Health: `curl http://127.0.0.1:7778/health` returns JSON with players online, sessions, tick time
(avg/max ms over the last second), DB queue depth, RSS, uptime, and the TLS fingerprint.

### Login security

Passwords never travel over the ENet game connection. The client:

1. connects over ENet and receives the server's TLS certificate, the login port, and a per-connection nonce;
2. checks the certificate against the fingerprint it pinned for that server address
   (on first contact it shows the fingerprint and asks the player to trust it, like SSH);
3. sends the username and password over TLS (TCP 7779) to that exact certificate;
4. receives a single-use ticket (valid 30 s, bound to its connection's nonce) and presents it over ENet.

On first start the server generates a self-signed P-256 certificate in `data/tls/` (in Docker:
the `emberfall-data` volume) and logs its SHA-256 fingerprint. **Publish that fingerprint** so
players can check it on first connect, and **back up `server.key`**: if it changes, every client
that pinned the old one refuses to log in until the player removes the old pin. To use your own
certificate, point `EMBERFALL_TLS_CERT`/`EMBERFALL_TLS_KEY` at it (clients still pin whatever it is).

### Docker

```sh
docker compose -f deploy/docker-compose.yml up -d --build
docker compose -f deploy/docker-compose.yml logs -f
curl http://127.0.0.1:7778/health
```

The image builds and runs the unit tests in its build stage, runs as an unprivileged user, keeps the
database, logs, and TLS identity in the `emberfall-data` volume, and publishes UDP 7777 (game) and
TCP 7779 (TLS login) on all interfaces, plus TCP 7778 (health) on localhost only.
`docker compose stop` sends SIGTERM, so characters are saved. To restrict the game and login ports
to one host address, set `EMBERFALL_GAME_BIND` in `deploy/.env` (see `deploy/.env.example`).
Get the fingerprint to publish with `docker logs emberfall-server | grep fingerprint`.

### Test tools

```sh
# Full scenario: register/login, create character, enter world, walk, chat, log out.
./build/release/tools/bot/emberfall-bot --host 127.0.0.1 --user bot1 --name Botone --duration 10
# Version mismatch must be rejected with a clear message.
./build/release/tools/bot/emberfall-bot --protocol-version 99 --expect-version-reject
# Send malformed packets, then verify the server still handshakes.
./build/release/tools/bot/emberfall-bot --fuzz 5000
# Combat: walk to a level-appropriate camp (read from --content), kill and loot 6 mobs.
./build/release/tools/bot/emberfall-bot --user fighter --name Fightor --fight 6 --duration 240 --content content
# Kill and loot 3 mobs, then die to a much stronger passive mob, respawn, walk back, and loot the corpse.
./build/release/tools/bot/emberfall-bot --user doomed --name Doomed --die --fight 3 --duration 240 --content content
# Everything above against a fresh server, including SIGTERM shutdown:
bash tools/scripts/live_test.sh build/asan
```

## Client (Godot 4.7, Windows .exe)

The client uses **Godot 4.7.2** (any 4.3+ should work; CI pins 4.7.2) with the Compatibility
(OpenGL 3) renderer for the widest Windows hardware support. It talks to the server through Godot's
low-level `ENetConnection`, not the high-level multiplayer API.

### Export templates

Exporting needs the export templates that match the editor version exactly.

- **Editor UI:** Editor > Manage Export Templates > Download and Install.
- **Command line (Linux):** `bash tools/scripts/install_godot.sh ~/godot 4.7.2` downloads the editor
  and templates, verifies their SHA-512 sums, and installs the templates into
  `~/.local/share/godot/export_templates/4.7.2.stable/`.
- **Windows:** templates live in `%APPDATA%\Godot\export_templates\4.7.2.stable\`; install them from
  the editor, or unzip the `.tpz` from the Godot release page there (`templates/*` contents).

### Export from Linux

```sh
godot --headless --path client --import     # first run only: builds the .godot import cache
godot --headless --path client --export-release "Windows Desktop" build/EmberfallClient.exe
```

Output: `client/build/EmberfallClient.exe`, a single portable executable (the PCK is embedded).
Godot 4.7 writes the icon and version info into the PE itself, so rcedit/wine are not needed.

### Export from Windows

Same commands in PowerShell with the Windows editor binary:

```powershell
& "C:\Tools\Godot\Godot_v4.7.2-stable_win64.exe" --headless --path client --export-release "Windows Desktop" build/EmberfallClient.exe
```

Or open `client/project.godot` in the editor and use Project > Export > Windows Desktop.

### Running the client

Start `EmberfallClient.exe`, enter the server address (`host` or `host:port`, default port 7777),
then register or log in.

| Action | Keys |
|---|---|
| Move | W/S move, A/D turn, Q/E strafe; hold right mouse to mouselook (A/D strafe while held); wheel zooms |
| Target | Tab / Shift+Tab cycle nearby mobs, left-click anything, F1 yourself, Esc clears |
| Fight | R toggles auto-attack, 1-0 use hotbar slots, C considers the target |
| Windows | K abilities (drag onto the hotbar; right-click a slot to clear it), I pack, L loots a targeted corpse |
| Other | X sits/stands, Enter chats, Esc for the menu |

Chat commands: `/sit`, `/stand`, `/loc`, `/con`, `/attack`, `/target NAME`. The chat window has a
Combat tab with the combat log. When you die, press *Respawn at bind point*, walk back to your
corpse (it keeps your coin and pack for an hour), target it, and press L to loot it.

**First connection:** the client shows the server's certificate fingerprint and asks you to trust it.
Compare it with the one the server admin published. Pins are stored in `known_servers.cfg` in the
client's user data folder (`%APPDATA%\Godot\app_userdata\Realms of Emberfall\`); delete a line there
if a server legitimately changed its certificate.

**SmartScreen:** the build is not code-signed, so Windows SmartScreen will warn "Windows protected
your PC" on first launch. Choose *More info > Run anyway*. Signing comes with release packaging.

### Client autotest mode

The real client can drive itself, which CI and manual verification use:

```powershell
.\EmberfallClient.exe -- --autotest --server 192.168.1.23:7777 --user tester --pass testpassword `
  --char Tester --walk 10 --say "hello" --expect-peers 1 --screenshot shot.png --result result.json
```

It registers (or logs in), creates the character if missing, enters the world, walks a circle,
chats, saves a screenshot, writes a JSON result (players seen, snapshots, corrections, chat log,
server fingerprint), camps, and exits with code 0 on success. Autotest trusts a new server
automatically and keeps its pins in a separate `autotest_known_servers.cfg` (`--reset-pins` clears
it). `--expect-error TEXT` makes a run pass only if login fails with that text.

Combat runs use the same UI code a player does (targeting, hotbar, loot window, death dialog):

```powershell
.\EmberfallClient.exe -- --autotest --server 192.168.1.23:7777 --user fighter --pass testpassword `
  --char Fighter --fight 3 --camp 50,0 --die-at -51.4,191.2 --timeout 400 `
  --screenshot combat.png --death-screenshot death.png --show-windows --result result.json
```

`--fight N` kills and loots N mobs at or below your level near `--camp`; `--die-at X,Z` then walks
there, picks a fight five or more levels above you, presses Respawn, walks back, and loots the
corpse. The result adds kills, deaths, corpses looted, level, XP, inventory, and a combat-log sample.
`tools/scripts/client_e2e.sh` runs two headless clients plus one combat run against a fresh server on Linux, then
restarts the server with a new certificate and checks that the client refuses it.
