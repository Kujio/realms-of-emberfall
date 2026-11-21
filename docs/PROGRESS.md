# Progress

Work one phase at a time (see CLAUDE.md). At the end of each phase, record **Done / Stubbed / Next** under it.

## Phase checklist

Phases follow the prompt pack in `emberfall_prompts.md`.

- [x] Phase 0: Repository setup (monorepo layout, CLAUDE.md, docs, .gitignore/.gitattributes, first commit)
- [x] Phase 1: Foundations: connect, log in, walk around (server skeleton, protocol + generator, accounts/characters, test zone with interest management and validated movement, Godot client with prediction/interpolation and say chat, bot, tests, Docker, Windows export, CI)
- [ ] Phase 2: Core combat and leveling, Warrior only (stats/rules, mobs + AI + threat, auto-attack/GCD, ~10 Warrior abilities, XP to 60, death/corpses/loot basics, combat UI)
- [ ] Phase 3: Spell engine with Cleric and Wizard (cast state machine, effects, resists, buffs/debuffs/stacking, scrolls/spellbook/gems, trainers, spell UI)
- [ ] Phase 4: All 14 classes, pets, skills, threat (pets, Bard songs, positionals, feign/sneak/charm/illusions, skills and proficiencies, final races, balance simulator v1)
- [ ] Phase 5: Items, equipment, vendors, bank, trade (600+ items, equipment stats/procs, loot tables and group loot, vendors, bank, secure trade, auction board)
- [ ] Phase 6: World content (6a zone pipeline + procedural dungeons + travel, 6b hubs + 12 outdoor zones, 6c 8 dungeons, 6d 150+ mobs + factions + NPCs, 6e quests + class epics, 6f map/journal/dialogue UI, 6g content validation)
- [ ] Phase 7: Social systems (all chat channels, groups, guilds, friends, /who and slash commands, GM commands with audit log and bans)
- [ ] Phase 8: Polish, balance, load test, release (balance sim 1-60, 100+ bot load test, robustness, client settings/polish, installer + release CI, README/DEPLOY.md, smoke test, tag v0.1.0)

## Phase notes

### Phase 1: Foundations: connect, log in, walk around (completed 2026-11-20)

**Verified end to end:** `emberfall-server` running in Docker on a Linux host (Gentoo, Docker 29) with two
`EmberfallClient.exe` instances on Windows 11. Both registered, created characters, entered
The Proving Flats, saw each other move, and exchanged say chat. A second run logged into the existing
accounts. Results: 0 movement corrections, ~19.5 inputs/s, 235/236 inputs acknowledged at report time.
Version mismatch (v0 and v99) is rejected with "Please update your client". 3,000 network-fuzzed
packets plus 400k in-process mutated packets did not crash the server.

**Done**
- Server: CMake presets (debug, release, asan), pinned FetchContent dependencies, JSON config with
  `EMBERFALL_*` env overrides and fail-fast validation, spdlog console + rotating file, ENet host
  (2 channels, packet size cap, per-IP connection cap), 20 Hz fixed-tick loop, DB worker thread,
  SIGTERM/SIGINT graceful shutdown that saves everyone, `/health` JSON endpoint.
- Protocol: `protocol/messages.json` -> `tools/gen_protocol.py` -> C++ header + GDScript codec;
  little-endian, bounds-checked, strict (UTF-8, finite floats, array caps, no trailing bytes);
  version handshake (now protocol v2); `--check` mode for CI. Per-client token-bucket rate limiting,
  violation counting and kicks, direction/channel/state checks on every message.
- Login security: passwords travel only over a TLS 1.2+ login service (OpenSSL, TCP 7779) to a
  self-signed certificate that clients pin on first use (fingerprint prompt; a changed certificate is
  refused). The service returns a single-use ticket bound to the ENet connection's nonce, and
  the client redeems it over ENet. The login service has per-IP rate limits, I/O deadlines, and a bounded worker pool.
- Accounts and characters: SQLite (WAL) behind a `Repository` interface; Argon2id (libsodium)
  register/login on the DB thread; one session per account; bad-ticket lockout; character
  list/create/delete with name rules, race/class/city validation, 4-per-account limit; position save
  on logout/disconnect/shutdown and 60 s autosave.
- Zone: one test zone (`proving_flats`), spatial-hash interest manager with hysteresis,
  spawn/despawn, intent-based movement with input budget (no speed hacks), teleport and desync
  detection with authoritative corrections, unreliable snapshots on channel 1, say chat by radius.
- Godot client (4.7.2, GDScript, static typing): login/register with editable server address,
  TLS login with trust-on-first-use certificate pinning, character select/create/delete, world with third-person camera, client-side prediction +
  reconciliation, interpolated remote players, chat window, HUD, Esc menu, `--autotest` mode.
- CodeRabbit scan (end of phase): 5 major findings, all addressed. Cleartext credentials were fixed
  by the TLS login service. Also fixed: the character-delete confirmation could delete a different
  character after a list refresh; the native run command in BUILD.md used container paths; the
  health endpoint had no whole-request deadline (slow-client stall); compose published the cleartext
  login port everywhere (moot once TLS landed; `EMBERFALL_GAME_BIND` can still restrict it).
  A second scan after these changes found 10 more (2 major, 8 minor), all fixed:
  - TLS and health listen sockets made non-blocking (accept race could hang shutdown).
  - Autotest `--expect-error` could pass on a successful login; it could also skip character creation after a slow frame, and it ignored a failed result write.
  - `max_players` is now enforced at world entry.
  - The GDScript string encoder bounds characters before trimming bytes.
  - The silent-client TLS test now requires a server-side close.
  - The bot checks max displacement rather than the endpoint, and fails if `--delete-after` doesn't delete.
  - The test scripts clean up their servers on interrupt.
  A third, final scan found 1 major issue, now fixed: the TLS login server's stop flag wasn't set
  under the queue mutex, so a worker could miss the wakeup and hang shutdown (and the save after it).
- Tooling: `emberfall-bot` (scenario with TLS login, `--expect-version-reject`, `--fuzz`), 70 Catch2
  tests (protocol, randomized/mutation fuzzing, movement, spatial hash, rate limiter, auth, DB,
  config, content, zone, full server sessions incl. malformed-packet storm, tickets, TLS identity,
  TLS login service incl. pin mismatch, garbage input, and per-IP limits, max_players), `tools/validate_content.py`,
  `tools/scripts/live_test.sh`, `tools/scripts/client_e2e.sh`, `.clang-tidy`.
- Build/deploy: multi-stage Dockerfile (runs tests in build stage, non-root runtime), compose file,
  Windows export preset with embedded PCK and icon, scripted export, CI workflow (server build +
  tests + ASan/UBSan + live bot test + headless client e2e + Docker image; Windows export artifact).
- Warning-free and all tests passing on GCC 16, GCC 13 (Ubuntu 24.04 Docker), and Clang 22.

**Stubbed / placeholder**
- Content: 3 placeholder races (Highlander, Vaelith, Deepkin), 14 classes with descriptions only,
  3 starting cities that all spawn into the single test zone. No stats yet.
- Character creation options are sent by the server (`CreateOptions`) instead of a client-bundled
  content copy.
- `admin_password` config and the `is_admin` account flag exist but no GM commands use them.
- Placeholder art: capsules with facing markers and name tags, grid ground, boundary posts.
- Health endpoint is stats only (no admin actions).
- `.clang-tidy` is clean (0 findings with clang-tidy 22) but not yet enforced in CI.

**Known gaps**
- Game traffic after login (movement, chat) is not encrypted. Passwords are protected, but an on-path
  attacker could hijack a live session. See DECISIONS.md.
- Lost movement packets cause small server-side snap-backs (no input redundancy yet).
- No client/server clock-drift compensation beyond the input budget.

**Next**
- Phase 2 (core combat and leveling, Warrior) builds on the zone, session, and protocol layers here.
- Movement input redundancy (resend the last few intents in each `MoveInput`) to remove snap-backs under packet loss.
- Enforce clang-tidy in CI. The systemd unit, backup script, and DEPLOY.md belong to Phase 8.
