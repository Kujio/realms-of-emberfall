# Realms of Emberfall

An original fantasy online role-playing game in the spirit of the classic tab-target MMOs: fourteen
classes, levels 1 to 60, groups of six, guilds, dungeons with bosses, corpse runs, factions, and a
world of hubs, wilds and dungeons connected by zone lines, ferries and portals.

> **An experiment with Claude Code.** This project is an experiment in building a complete game with
> [Claude Code](https://claude.com/claude-code), Anthropic's AI coding agent. The code, content, tests,
> tools and documentation were written by Claude Code, one phase at a time, from a set of prompts
> (the working rules it follows are in [CLAUDE.md](CLAUDE.md); what each phase delivered is in
> [docs/PROGRESS.md](docs/PROGRESS.md)). A person set the direction, played the builds and reported
> what was wrong. Expect the rough edges of an experiment: placeholder art, simulated balance, and no
> live player base yet. It is an original game, not affiliated with or derived from any existing one.

- **Server:** authoritative C++20, one process, 20 Hz, ENet over UDP, SQLite, TLS logins. Runs on Linux
  (Docker or systemd).
- **Client:** Godot 4 (GDScript), third-person low-poly 3D. Ships as one Windows `.exe` (and an
  installer).
- **Content:** all data-driven JSON under `content/`: 14 classes with 25-30+ abilities each, 160+ mob
  types, 700+ items, 27 zones, 81 quests with an epic chain per class.

Version 0.1.0. Art is placeholder until the Phase 9 art pass (see [ASSETS.md](ASSETS.md)).

## Playing

1. Download `EmberfallSetup-<version>.exe` (installer) or `EmberfallClient.exe` (portable, no install)
   from the release page.
2. Windows SmartScreen warns about unsigned programs: choose **More info → Run anyway**.
3. Enter the server address your server's admin gave you (`host` or `host:port`, default port 7777).
   The login screen shows whether the server is up, how many are playing, and its message of the day.
4. **Register** an account (username: 3-16 letters, digits or underscores, starting with a letter;
   password: 8-64 characters), or **Log In**.
5. The first time you connect, the client shows the server's certificate fingerprint. Compare it with
   the one the admin published, then trust it. Your password only ever travels over that encrypted
   connection.
6. Create a character (race, class, starting city) and enter the world.

Keys (all rebindable under **Settings → Keys**): W/A/S/D or arrows to move, Q/E strafe, right mouse to
mouselook, Tab to target, R auto-attack, 1-0 the hotbar, X sit (much faster regeneration), C consider,
T talk, L loot, I inventory, B spellbook, K abilities, P group, G guild, O friends, M map, U journal,
Enter to chat (`/s`, `/sh`, `/ooc`, `/t name`, `/g`, `/gu`), Esc for the menu. Settings also has window
mode, resolution, vsync and volumes.

## Running a server

The short version (Docker on Ubuntu; ports 7777/udp and 7779/tcp open):

```bash
git clone <your-repository-url> emberfall && cd emberfall
docker compose -f deploy/docker-compose.yml up -d --build
docker compose -f deploy/docker-compose.yml logs | grep fingerprint     # publish this for players
docker exec emberfall-server emberfall-server --grant-admin youraccount # after registering in the client
```

[docs/DEPLOY.md](docs/DEPLOY.md) is the full guide: a fresh VPS, Docker or systemd, the firewall,
configuration, logs, backups by cron, restoring, updating without data loss, monitoring and measured
capacity. Game master tools are in [docs/ADMIN.md](docs/ADMIN.md).

## Building from source

```bash
# Server (Linux: GCC 12+ or Clang 15+, CMake 3.22+, Ninja, libsodium, OpenSSL)
cmake --preset release && cmake --build --preset release && ctest --preset release
build/release/server/emberfall-server            # starts with defaults; --help lists options

# Everything at once, end to end (build, start, level a bot, loot, trade, log out)
bash tools/scripts/smoke_test.sh

# Client (Godot 4.7 with export templates)
godot --headless --path client --import
godot --headless --path client --export-release "Windows Desktop" build/EmberfallClient.exe
```

[docs/BUILD.md](docs/BUILD.md) covers presets (debug, release, asan, fuzz), the test tools, the client's
export from Linux or Windows, and the installer.

## Documentation

| | |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how it fits together: threads, the tick, protocol, persistence, client |
| [docs/DEPLOY.md](docs/DEPLOY.md) | running a server |
| [docs/ADMIN.md](docs/ADMIN.md) | game masters, bans, the audit log |
| [docs/CONTENT.md](docs/CONTENT.md) | adding spells, items, mobs, zones and quests |
| [docs/ZONES.md](docs/ZONES.md) | the zone format, dungeons and travel |
| [docs/MODELS.md](docs/MODELS.md) | adding character and creature models |
| [docs/BUILD.md](docs/BUILD.md) | building, testing, exporting |
| [docs/DECISIONS.md](docs/DECISIONS.md) | design decisions and balance numbers |
| [docs/PROGRESS.md](docs/PROGRESS.md) | what each phase delivered |

## Repository layout

`server/` the game server and its tests · `client/` the Godot client · `protocol/` the message
definitions · `content/` game data · `tools/` generators, validators, bot, simulator, fuzzers, scripts ·
`deploy/` Docker, systemd, backups, installer · `docs/` · `.github/workflows/` CI and releases.
