# Architecture

Realms of Emberfall is an authoritative client/server game. The client sends intents (move this way,
target that, cast gem 3); the server validates and simulates everything and tells the client what
happened. Nothing the client says is trusted beyond "this is what the player asked for".

```
 Godot 4 client (GDScript)                         emberfall-server (C++20, one process)
 ┌──────────────────────────┐   UDP 7777 (ENet)   ┌─────────────────────────────────────────────┐
 │ Net (ENetConnection)     │◄──────────────────► │ main thread: 20 Hz loop                     │
 │ Session (login flow)     │  sealed after login │   ENet poll → SecureTransport → GameServer   │
 │                          │  ch0 reliable       │     → Zones → flush                          │
 │ World (prediction,       │  ch1 unreliable     │ TLS login threads (4): passwords, tickets    │
 │  interpolation, HUD)     │  ch2 TLS-only msgs  │ DB worker thread: SQLite (WAL), Argon2id     │
 │ Settings, Audio          │   TCP 7779 (TLS)    │ health thread: GET /health on 127.0.0.1:7778 │
 └──────────────────────────┘◄──────────────────► └─────────────────────────────────────────────┘
```

## Repository

| path | |
|---|---|
| `server/src/core` | config (file + `EMBERFALL_*` overrides), logging, tick statistics |
| `server/src/net` | ENet transport, packet sealing (`packet_crypto`, `secure_transport`), rate limiters, byte buffers, the generated protocol, TLS login |
| `server/src/auth` | Argon2id passwords, login tickets, name rules, bans |
| `server/src/db` | the `Repository` interface, its SQLite implementation and migrations, the DB worker |
| `server/src/game` | content loading and validation, pure rules, zones (the simulation), the game server |
| `server/src/http` | the health endpoint |
| `server/tests` | Catch2: rules, content, zones, the wire (a fake transport), the database, fuzzing |
| `client/scripts` | the Godot client (`net/`, `ui/`, `world/`, `session.gd`, `settings.gd`, `audio.gd`) |
| `protocol/messages.json` | the one message definition both sides are generated from |
| `content/` | all game data (see [CONTENT.md](CONTENT.md)) |
| `tools/` | generators, validators, the bot, the balance simulator, fuzzers, test scripts |
| `deploy/` | Dockerfile, compose, systemd unit, backup script, Windows installer |

## Threads

- **Main (game) thread.** Everything that touches game state: the fixed-rate loop polls ENet,
  dispatches packets to `GameServer`, ticks every loaded zone, and flushes. Between ticks it sleeps in
  `enet_host_service` until the next tick is due. Nothing on this thread blocks on I/O.
- **DB worker.** One thread owns the SQLite connection. The game thread posts jobs (load a character,
  save a batch, create a guild) and drains completions at the start of each tick, so results are
  applied on the game thread. Argon2id hashing also runs here, off the tick.
- **TLS login service.** A small pool accepts TLS connections on 7779, reads one `TlsLogin`, asks the
  DB worker to verify or create the account, and answers with a one-use ticket bound to the game
  connection's nonce, plus a random session key for that connection. Per-IP rate limits apply before
  any work.
- **Health.** A tiny HTTP/1.0 server reading atomics the game thread publishes once a second.

## The tick (20 Hz, 50 ms budget)

`GameServer::tick` drains DB completions, then ticks each zone, moves players between zones (zone lines,
portals, deaths), unloads idle zones, closes kicked sessions, expires lingering characters and
half-open logins, and every second flushes saves. `Zone::tick` runs in phases (reported by
`/health` as `tick_phase_ms` and printed by the load test):

1. **players** — buff/DoT ticks, queued movement inputs (validated), casting and songs.
2. **mobs** — effects, AI (idle, aggro, chase, leash, flee) and pathing (A* on a nav grid, budgeted).
3. **combat** — auto-attack swings, avoidance and mitigation, procs, threat, deaths.
4. **world** — respawns, corpse decay, loot rolls, trades, vendors, travel, regen.
5. **interest** — each player's visible set from a spatial hash (with hysteresis); spawns/despawns.
6. **snapshots** — per player, the entities that changed (or have not been sent for a second) on the
   unreliable channel. Each entity's entry is built once per tick and shared by every watcher.
7. **state** — per-player stats, inventory, sheet, factions and journal, only when changed.

Game rules (combat math, spell resolution, XP and loot) are pure functions in `rules.cpp`,
`spells.cpp`, `items.cpp`, `quests.cpp` and friends, unit-tested on their own; zones call them.

## Network and protocol

- **ENet** over UDP. Channel 0 is reliable and ordered (commands, chat, inventory); channel 1 is
  unreliable (movement inputs and snapshots). The client uses Godot's low-level `ENetConnection`, not
  the high-level multiplayer API, so the wire format is entirely ours.
- **`protocol/messages.json`** defines every message: id, direction, channel and typed fields with
  bounds (string and array maxima). `tools/gen_protocol.py` writes `server/src/net/protocol.gen.hpp`
  and `client/scripts/net/protocol.gd`; CI fails if either is stale. Encoding is little-endian binary:
  a u16 id, then the fields. Decoding is strict (every bound checked, the payload fully consumed); a
  malformed packet is dropped and counts as a violation, never a crash.
- **Handshake.** `Hello{protocol_version}` → `HelloResult` (a mismatch gets a plain "please update your
  client" message and a disconnect) and `ServerStatus` (message of the day, players online). The login
  screen also probes servers this way, on its own connection, before anyone logs in.
- **Login.** The password goes only to the TLS service, whose certificate the client pins on first use
  (like SSH). The service returns a ticket and a session key; the game connection redeems the ticket
  (`TicketLogin`), so the UDP channel never carries a password.
- **Sealed packets (Phase 10).** From the ticket login on, every packet both ways is encrypted and
  authenticated with the session key: a counter, AES-256-CTR ciphertext, and a 16-byte HMAC-SHA256
  tag, with separate keys per direction and channel. `SecureTransport` sits between ENet and
  `GameServer`. It seals what the game server sends, and checks the tag and counter of each arriving
  packet before the game server sees it. A bad reliable packet closes the connection. The client's
  `Net` does the same (`net/packet_crypto.gd`). See DECISIONS.md "Game traffic encrypted after login".
- **Abuse limits.** Per session: a packet and byte token bucket, separate buckets for chat, commands,
  combat actions and item moves, a violation counter that kicks, and a login timeout for connections
  that never authenticate. Per IP: connection count and TLS login attempts.

## Movement and anti-cheat

The client predicts its own movement with the same step function the server runs (`step_position`:
speed, terrain height, 2D collision against circles, boxes and dungeon tiles) and sends inputs with
sequence numbers. The server applies them at most a few per tick (an input budget), rejects implausible
displacement (teleports, speed), corrects the client when they disagree (`MoveCorrection`), and acks
the last applied sequence in every snapshot so the client can replay. Across a zone change the client
continues its count 200 higher and the new zone drops anything below that, so inputs the old world had
in flight cannot stall the new one. Everything else is checked
server-side too: ranges (melee, spells, loot, trade, NPCs), cooldowns and the global cooldown, line of
sight, resources, ownership (corpses, items), and state (no casting while stunned or dead). A second login of the same account replaces the first; a connection dropped mid-fight leaves
the character in the world for `linger_seconds`, so disconnecting is never an escape, and logging back
in picks the same character up where it stands.

## Persistence

`db::Repository` is the storage interface (SQLite today, written to be swappable for Postgres).
Characters are saved in batches in one transaction: every 60 s, on logout, zone change, death and
trade, and on shutdown. A failed batch is kept and retried with the next one, so related changes (both
sides of a trade, a listing and its seller) are never stored apart. The schema has a version and
migrates forward at startup; a quick integrity check runs before any migration, and a damaged file stops
the server instead of being written to (see [DEPLOY.md](DEPLOY.md) for backups and restores).

## Content

`load_content` reads every file in `content/`, resolves references and validates ranges, collecting
every problem before failing. `tools/validate_content.py` runs the same kinds of checks (plus world
reachability, quest and loot sanity) without a build, and its `--self-test` proves it catches broken
copies. The client bundles a display copy (`client/data/`) written by `tools/sync_client_content.py`.

## Client

- `Net` (autoload): the ENet connection and the generated codec, emitting decoded messages.
- `Session` (autoload): the login → character select → world flow, the certificate pin store.
- `Settings` (autoload): window mode, resolution, vsync, the mixer, and rebindable keys (Godot
  `InputMap` actions; defaults in `keybinds.gd`), saved in `user://options.cfg`.
- `Audio` (autoload): named sound events from `data/sounds.json` on the Music, SFX and UI buses.
- `main.gd`: swaps screens, shows the loading screen while a world builds and a dialog when the
  connection ends.
- `world/`: the zone (built from the same zone file and seed the server uses), the predicted local
  player, interpolated remote entities with data-driven models and animation, the HUD and windows.

## Testing

- **Catch2** (`ctest`): ~310 tests: rules, content, zones, every wire flow on a fake transport, the
  database, reconnects, fuzzing of the decoder. Built warning-free under GCC and Clang, and under
  ASan/UBSan in CI.
- **libFuzzer** (`cmake --preset fuzz`): `emberfall-fuzz-protocol` (decoder and round trips) and
  `emberfall-fuzz-server` (a live in-world session fed fuzzed packet streams).
- **Live tests** with the C++ bot: `tools/scripts/live_test.sh` (scenarios), `smoke_test.sh`
  (login → create → level → loot → trade → logout), `load_test.sh` (a swarm across zones).
- **Client**: `tests/test_runner.gd` (headless checks) and `tools/scripts/client_e2e.sh` (two headless
  clients playing against a real server).
- **Balance**: `emberfall-balance-sim --progression` (time to level per class, solo and grouped).
