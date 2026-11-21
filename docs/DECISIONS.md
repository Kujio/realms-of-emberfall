# Decisions Log

Record significant decisions here, newest first.

Format: `## YYYY-MM-DD — Title` followed by **Decision**, **Rationale**, **Alternatives considered**.

## 2026-11-20 — Phase checklist follows the prompt pack

**Decision:** docs/PROGRESS.md lists Phases 0-8 exactly as defined in `emberfall_prompts.md`.
**Rationale:** The initial checklist was a guess written before the phase prompts were available.

## 2026-11-20 — Login over TLS with pinned certificates; OpenSSL added (protocol v2)

**Decision:** Passwords travel only over a TLS 1.2+ TCP login service (port 7779), never over ENet.
The server uses OpenSSL 3 from the system (`libssl-dev`), which adds to the dependency list in CLAUDE.md.
The flow:
1. `HelloResult` (ENet) carries the server certificate, the login port, and a random per-connection nonce.
2. The client checks the certificate against its pin for that server address (trust on first use
   with a fingerprint prompt, like SSH; a changed certificate is refused).
3. The client sends `TlsLogin` over TLS to exactly that certificate.
4. The server verifies the password with Argon2id on the DB worker and returns a single-use ticket
   (30 s TTL, bound to that connection's nonce).
5. The client presents the ticket with `TicketLogin` over ENet.

The server generates a self-signed P-256 certificate on first start, keeps it in `data/tls/` (key
mode 0600), and logs its SHA-256 fingerprint (also shown on `/health`). The login service caps
per-IP attempts (burst 10, then 12/min) and applies whole-request I/O deadlines. It runs on 4 worker
threads with a bounded queue. The TLS-only messages use channel id 2, so the ENet channel check rejects them there.
**Rationale:** The user chose a vetted TLS library over hand-rolled crypto. Godot ships an mbedTLS
client with certificate pinning (`TLSOptions.client(cert)`), and OpenSSL on the server is a standard
package. Pinning avoids needing a CA-issued certificate for LAN and self-hosted servers.
**Alternatives considered:** Hand-written X25519/ChaCha20 in GDScript (rolling our own crypto);
a libsodium GDExtension (a native Windows build per client); ENet DTLS (lsalzman ENet has none);
CA-only verification (unusable for self-hosted servers without a domain).
**Remaining exposure:** Game traffic after login (movement, chat) is unencrypted, as in most MMOs.
An on-path attacker could still hijack an already-logged-in ENet session but cannot learn
passwords. Full session encryption would be a separate decision.
**History:** CodeRabbit's Phase 1 scan flagged cleartext credentials. A LAN-only client guard and a
loopback-only Docker bind were added as stopgaps, then removed once TLS login landed.

## 2026-11-20 — Godot 4.7.2 and the Compatibility renderer

**Decision:** Use Godot 4.7.2 (latest stable at the time) and `gl_compatibility`.
**Rationale:** CLAUDE.md requires 4.3+. 4.7 writes the Windows icon and version info into the
exported .exe itself (no rcedit/wine), which makes exporting from Linux and CI simpler. The
Compatibility (OpenGL 3) renderer runs on the widest range of Windows hardware, and low-poly art doesn't need Forward+.
**Alternatives considered:** Pinning 4.3; Forward+ renderer.

## 2026-11-20 — Dependencies: FetchContent (pinned + hashed), libsodium and OpenSSL from the system

**Decision:** ENet 1.3.18, nlohmann/json 3.12.0, spdlog 1.15.3, SQLite 3.50.4 (amalgamation), and
Catch2 3.8.1 come in through FetchContent with SHA-256 hashes. libsodium (pkg-config) and OpenSSL 3
(`find_package(OpenSSL 3.0)`) come from the system packages (`libsodium-dev libssl-dev` /
`dev-libs/libsodium dev-libs/openssl`).
**Rationale:** No extra tooling to bootstrap in Docker or CI; reproducible versions. libsodium has
no CMake build, OpenSSL is security-critical and should get distro security updates, and both are
packaged everywhere. ENet and SQLite are compiled as our own targets because
ENet's CMakeLists predates CMake 3.5 (CMake 4 rejects it).
**Alternatives considered:** vcpkg manifest mode (heavier bootstrap, slower cold builds).

## 2026-11-20 — Protocol definition format and codec rules

**Decision:** `protocol/messages.json` (JSON, not YAML) is the single source. The wire format is a u16
message id followed by fields in order, little-endian. Strings are a u16 byte length plus UTF-8 with
a per-field max. Arrays are a u16 count with a per-field max. Decoders reject: overruns, oversize
strings/arrays, invalid UTF-8 (incl. overlongs/surrogates), NaN/Inf floats, bools other than 0/1,
and trailing bytes. Encoders never fail: they truncate strings on a UTF-8 boundary and clamp arrays,
so the server can never emit undecodable data. Each message declares its direction and channel, and
the server enforces both.
**Rationale:** The generator needs only the Python standard library. Strict decoding is the main
defence against malformed input.
**Alternatives considered:** YAML (needs PyYAML), FlatBuffers/protobuf (less control over GDScript output).

## 2026-11-20 — Movement: intents + prediction + reconciliation

**Decision:** Every 50 ms (one server tick) the client sends a sequenced intent: a move direction of
length ≤ 1, a facing, and its predicted position. The server applies intents with the same pure
`step_position` the client uses for prediction. A per-player input budget (one token per tick, up to 4
banked) caps simulation at one step per tick, so flooding cannot speed a player up. Two checks
trigger a reliable `MoveCorrection`: client predictions that jump implausibly between inputs
(teleport, also counted as a violation), and predictions that drift more than 2 m from the server
(desync). Checks are suppressed for 10 ticks after a correction. Snapshots carry the last applied
sequence number, so the client rewinds to the server position and replays its unacknowledged
intents. Remote players render 100 ms in the past by interpolation.
**Rationale:** CLAUDE.md says clients send intents and the server simulates. This makes speed hacks
impossible by construction, while teleport/desync checks still catch broken or tampered clients.
**Alternatives considered:** Client-authoritative positions with speed checks (EverQuest-style, easier
to cheat).

## 2026-11-20 — Client timing uses the wall clock; delta smoothing off

**Decision:** The client paces prediction steps and its server-time estimate with
`Time.get_ticks_usec()`, and `application/run/delta_smoothing` is disabled.
**Rationale:** On a high-refresh monitor (200 Hz) with the window presenting at 60 fps, Godot's
delta smoothing reported deltas about 3.3x too small, so the client simulated at ~1/3 speed (found in
Windows testing).

## 2026-11-20 — Interest management

**Decision:** A uniform spatial hash on XZ with cell size = the zone's interest radius (80 m in the
test zone). Spawn at ≤ r, despawn beyond 1.15 r (hysteresis). Spawns and despawns go on the reliable
channel; positions go in per-recipient unreliable snapshots that list only known entities.

## 2026-11-20 — Sessions and accounts (see also: Login over TLS)

**Decision:** Usernames are 3-16 chars `[a-z0-9_]`, case-insensitive. Passwords are 8-64 chars.
Character names are 4-15 ASCII letters, capitalized, unique case-insensitively. One session per
account: a new login kicks the older session (saving its character first). Five invalid tickets on
an ENet connection disconnect it, and the TLS login service rate-limits password attempts per IP.
Unknown usernames still pay for one Argon2id verify (timing equalization). Argon2id uses libsodium
INTERACTIVE limits and runs on the DB worker thread, so the game tick never blocks.
"Logout" from the world camps to character select; "Logout" at character select ends the session.

## 2026-11-20 — Abuse limits

**Decision:** Per client: 60 packets/s (burst 120) and 32 KiB/s (burst 64 KiB). Packets over the
limit are dropped, and 200 drops cause a kick. Ten protocol violations (malformed, unknown id, wrong
channel, wrong state, teleport) cause a kick. Max packet 4 KiB, 8 connections per IP, chat limited
to 1 message/s (burst 5). All values are in the config struct.

## 2026-11-20 — Character-creation options come from the server (Phase 1)

**Decision:** After login the server sends `CreateOptions` (races with allowed classes/cities,
classes, cities) built from `/content`.
**Rationale:** It keeps one source of truth until the content-sync pipeline exists (Phase 2). The server
still validates every choice.
**Alternatives considered:** Bundling a synced content copy in the client now.

## 2026-11-20 — Client UI built in code; `preload` instead of `class_name`

**Decision:** Screens are scripts that build their controls in `_ready()`; only `main.tscn` is a
scene file. Scripts reference each other through `preload()` constants rather than global
`class_name`s.
**Rationale:** Hand-written `.tscn` files are fragile, and the global class cache can be missing on a
fresh headless export. Real scenes will come with real art.

## 2026-11-20 — Ports and container layout

**Decision:** Game UDP 7777, TLS login TCP 7779, health TCP 7778. Compose publishes 7777/7779 on all
interfaces (`EMBERFALL_GAME_BIND` restricts them) and 7778 on 127.0.0.1 only. The container runs
as uid 10001, with the database, logs, and TLS identity in the `emberfall-data` volume.

## 2026-11-20 — Placeholder race names

**Decision:** Phase 1 races are Highlander, Vaelith, and Deepkin. An early draft used "Sylvari", which
is another game's race name, and it was replaced.
