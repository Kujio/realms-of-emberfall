# Building and running Realms of Emberfall

## Layout

| Path | What |
|---|---|
| `server/` | C++20 game server (`emberfall-server`) and its Catch2 tests |
| `tools/bot/` | Headless C++ test client (`emberfall-bot`) |
| `protocol/messages.json` | The single wire-protocol definition |
| `tools/gen_protocol.py` | Generates `server/src/net/protocol.gen.hpp` and `client/scripts/net/protocol.gd` |
| `tools/validate_content.py` | Validates `/content` JSON (CI runs it) |
| `content/` | Game data: rules, races, classes, cities, zones and spawns, skills, abilities, spells and songs, pets, items (including spell scrolls and summoned items), loot tables, mobs, NPC trainers |
| `tools/gen_classes.py` | Generates every class's abilities, spells and songs, the pets, scrolls, summoned items, trainers and the practice lockbox (output committed; `--check` for CI) |
| `tools/gen_spells.py` | The Cleric and Wizard spell lines (used by `gen_classes.py`; running it regenerates everything through `gen_classes.py`) |
| `tools/balance_sim/` | Balance simulator v1 (`emberfall-balance-sim`): every class solo-kills even-level mobs in a real headless zone; prints time to kill, win rate, health left and mana use, and flags outliers |
| `tools/gen_phase2_content.py` | Generates the placeholder mobs, loot tables, and test-zone spawns (output committed) |
| `content/models.json`, `content/appearance.json` | Models, animation maps and held-item visuals; race/gender and class looks (see [MODELS.md](MODELS.md)) |
| `tools/gen_models.py` | Generates the Emberfall Basic starter GLB models in `client/assets/models/emberfall_basic/` (output committed; `--check` for CI) |
| `tools/inspect_model.py`, `tools/convert_to_glb.sh`, `tools/godot_convert/` | List a model's clips/bones/meshes/materials; convert FBX/OBJ packs to GLB with Godot; measure heights and ground speeds (see [MODELS.md](MODELS.md)) |
| `tools/gen_items.py` | Generates the itemization: armor, weapons, shields, jewelry, ammo, consumables, bags, named dungeon placeholders, item effects (`content/item_effects.json`), every mob's loot pools, and the vendors, banker and auctioneer (output committed; `--check` for CI). Run it after `gen_classes.py` |
| `tools/sync_client_content.py` | Writes the client's bundled display data `client/data/visuals.json` and `client/data/items.json` (item names, stats and effects for tooltips) (output committed; `--check` for CI) |
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
cmake --preset fuzz    && cmake --build --preset fuzz     # libFuzzer targets (Clang + ASan/UBSan)
```

The fuzz preset builds `build/fuzz/tools/fuzz/emberfall-fuzz-protocol` (the decoder, with an
encode round trip) and `emberfall-fuzz-server` (a live in-world session fed fuzzed packet streams):

```sh
mkdir -p /tmp/fz && build/fuzz/tools/fuzz/emberfall-fuzz-server /tmp/fz -max_total_time=300 -max_len=4096
```

Database tools (the server never starts the game for these):

```sh
./build/release/server/emberfall-server --check-db                 # full integrity check, exit 0 = healthy
./build/release/server/emberfall-server --backup backups/now.db    # online, checked copy (safe while running)
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
# A caster: find the class trainer, buy and scribe the level-1 scroll it lacks, buff itself,
# memorize the new spell, then kill and loot 3 mobs with spells.
./build/release/tools/bot/emberfall-bot --user wizbot --name Wizbot --class wizard --caster --fight 3 --duration 240 --content content
# Everything above against a fresh server, including SIGTERM shutdown:
bash tools/scripts/live_test.sh build/asan
# Level to 3 (looting every kill), then trade coin for an item with a second bot:
./build/release/tools/bot/emberfall-bot --user smoker --name Smokey --fight 6 --until-level 3 --trade-with Partner --duration 600
./build/release/tools/bot/emberfall-bot --user partner --name Partner --trade-accept Smokey
# Load: 150 bots in one process (one thread and ENet host each), logging in 150 ms apart:
./build/release/tools/bot/emberfall-bot --user loadbot --swarm 150 --ramp-ms 150 --duration 120
```

Scripts (each starts its own server on a temporary database and cleans up):

| script | |
|---|---|
| `tools/scripts/smoke_test.sh [preset]` | build, start, then login → create → level to 3 → loot → trade → logout → re-login; fails on any error, sanitizer report or error log line |
| `tools/scripts/live_test.sh <build-dir>` | every bot scenario, the version mismatch and the fuzz bot |
| `tools/scripts/load_test.sh <build-dir> [bots] [seconds]` | a swarm over four zones; prints tick mean/p99, CPU, memory, bandwidth per player and the time per tick phase; fails above `P99_LIMIT_MS` (25) |
| `tools/scripts/client_e2e.sh <godot> <build-dir>` | two headless Godot clients against a real server |

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

### Installer (Inno Setup)

`deploy/windows/emberfall.iss` wraps the exported `.exe` into `EmberfallSetup-<version>.exe` (per-user
install by default, Start menu and optional desktop shortcuts, an uninstaller). On Windows with
[Inno Setup 6](https://jrsoftware.org/isinfo.php):

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 deploy\windows\emberfall.iss
```

Output: `deploy\windows\Output\EmberfallSetup-0.1.0.exe`. The portable `.exe` is always shipped too.
Neither is code-signed, so Windows SmartScreen shows "Windows protected your PC" the first time;
players choose **More info → Run anyway** (signing needs a code-signing certificate).

### Releases

Pushing a tag `vX.Y.Z` runs `.github/workflows/release.yml`: it exports the client, builds the
installer on a Windows runner, builds and smoke-tests the server image and pushes it to
`ghcr.io/<owner>/<repo>/emberfall-server:X.Y.Z`, then creates a GitHub release with the installer, the
portable `.exe`, the image as a `.tar.gz`, and `SHA256SUMS.txt`. The tag must match
`application/config/version` in `client/project.godot` (and the export preset's version fields).

### Running the client

Start `EmberfallClient.exe`, enter the server address (`host` or `host:port`, default port 7777),
then register or log in.

| Action | Keys |
|---|---|
| Move | W/S move, A/D turn, Q/E strafe; hold right mouse to mouselook (A/D strafe while held); wheel zooms |
| Target | Tab / Shift+Tab cycle nearby mobs, left-click anything, F1 yourself, Esc clears |
| Fight | R toggles auto-attack, 1-0 use hotbar slots, C considers the target |
| Spells | Click a gem above the hotbar to cast it (casters' keys 2-9 cast gems 1-8), Esc stops a cast; moving or being hit can interrupt it |
| Windows | K abilities and B spellbook (drag onto the hotbar or a gem; right-click to clear), J skills (with commands to drag onto the hotbar), I pack (Scribe on scrolls), L loots a targeted corpse, T talks to a targeted trainer, P group |
| Pets | The pet window (under your player frame) has Attack (your target), Follow, Guard (here), Back off and Dismiss |
| Other | X sits/stands (sitting meditates: faster mana), Enter chats, Esc for the menu |

Chat commands: `/sit`, `/stand`, `/loc`, `/con`, `/attack`, `/target NAME`, `/cast N`, `/stopcast`,
`/stopsong`, `/pet attack|follow|guard|back|dismiss`, `/book`, `/skills`. The chat window has a Combat tab with the combat log. When you die, press *Respawn at bind
point*, walk back to your corpse (it keeps your coin and pack for an hour), target it, and press L to
loot it; a Cleric can instead resurrect you at the corpse, which restores some of the lost XP.

**Spells:** new spell users (the pure casters and Bards) start with two spells or songs memorized and
25 copper; Paladins, Shadow Knights and Rangers learn their first spells at level 9. Every class that
uses spells has a trainer in a ring around the start (Sister Maelis for Clerics, Archmage Torvan Quill
for Wizards, and so on): click yours, press T, buy a scroll, then press *Scribe* on it in your pack (I), open the spellbook (B),
and drag the spell onto a gem. Your buffs show at the top right and your target's effects under the
target frame; hover them for details.

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

Caster runs (Cleric or Wizard) go through the spell UI:

```powershell
.\EmberfallClient.exe -- --autotest --server 192.168.1.23:7777 --user wizard --pass testpassword `
  --char Pyrelle --class wizard --caster 3 --camp 40,0 --timeout 300 --show-windows `
  --screenshot casting.png --result result.json
```

`--caster N` finds the class trainer, buys the level-1 scroll you lack, scribes it, casts the gem-2
buff on yourself, memorizes the new spell into gem 2, then kills and loots N mobs with spells,
standing still while casting (a Cleric heals itself below half health). The result adds casts
completed and interrupted, spells landed, resists, the scribed and memorized spell, your buffs,
and the most effects seen on a target.

Social runs (Phase 7) take two clients started together:

```powershell
.\EmberfallClient.exe -- --autotest --server 192.168.1.23:7777 --user lead --pass testpassword `
  --char Leora --class cleric --social lead --partner Joryn --guild "Emberfall Testers" --screenshot lead.png --result lead.json
.\EmberfallClient.exe -- --autotest --server 192.168.1.23:7777 --user join --pass testpassword `
  --char Joryn --social join --partner Leora --screenshot join.png --result join.json
```

The leader finds the partner with `/who all`, invites them, founds the guild and invites them into it. The
joiner accepts both through the invitation popup. Each speaks on the group and guild channels and must hear
the other, then opens the group, guild and friends windows. The screenshot of those windows is saved next to
`--screenshot` as `*_social.png`. The leader disbands the guild at the end, so the run can be repeated.

Every run also reports the models and animation states it saw (`visuals` in the result).
`--gender 0|1` picks female or male at creation; `--expect-models` fails the run if your own
character fell back to a placeholder; `--expect-anims idle,run,attack1,...` fails it unless every
listed animation state played; `--preview-screenshot PATH` captures the create screen's model preview.

`tools/scripts/client_e2e.sh` runs two headless clients plus one combat run against a fresh server
on Linux, then a Wizard and a Cleric caster run, each with model and animation expectations. It
then restarts the server with a new certificate and checks that the client refuses it.

### Client tests

Headless GDScript tests for the model and animation pipeline (the animation state machine and its
fallbacks, speed matching, look resolution, GLB loading, parts, tints, attachments, placeholders,
facing, and the character preview):

```sh
godot --headless --path client --import                          # once, so the GLB files are imported
godot --headless --path client --script res://tests/test_runner.gd  # exit code 0 = pass
```

`client/tests/` is excluded from the export.

### Content for the client

After editing `content/models.json`, `content/appearance.json`, or any item (by hand or with
`python tools/gen_items.py`), regenerate the client's copy (CI fails if it is stale), and validate:

```sh
python tools/gen_items.py          # only if you changed the generator
python tools/sync_client_content.py
python tools/validate_content.py   # also opens every GLB and checks clips, bones, parts and materials
```

## Balance simulator

```sh
./build/release/tools/balance_sim/emberfall-balance-sim                 # every class, levels 1-60, 12 fights per cell
./build/release/tools/balance_sim/emberfall-balance-sim --classes rogue,monk --levels 10,30,50 --fights 20
./build/release/tools/balance_sim/emberfall-balance-sim --strict        # exit 1 if any class is an outlier
./build/release/tools/balance_sim/emberfall-balance-sim --gearless      # bare-handed, as in Phase 4 (default: the recommended kit)
```

Options: `--content DIR`, `--seed N`, `--ttk-factor X` (outlier threshold against the median time to
kill, default 3), `--min-win R` (default 0.5). The kit each class plays comes from content alone (the
best rank of each line it knows), so a content change shows up in the table without code changes.
The latest table and the outliers it flags are in [DECISIONS.md](DECISIONS.md).

**Progression** (Phase 8): hours from level 1 to 60 per class, solo and in a standard six-person group,
by level band, with real resting between pulls:

```sh
./build/release/tools/balance_sim/emberfall-balance-sim --progression                    # every level, 4 pulls each
./build/release/tools/balance_sim/emberfall-balance-sim --progression --step 3 --detail  # faster; per-level minutes, fight and rest
./build/release/tools/balance_sim/emberfall-balance-sim --progression --mode group --classes enchanter,bard
./build/release/tools/balance_sim/emberfall-balance-sim --progression --factor 1.4 --strict   # what CI runs
```

