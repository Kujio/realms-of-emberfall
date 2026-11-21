# Decisions Log

Record significant decisions here, newest first.

Format: `## YYYY-MM-DD — Title` followed by **Decision**, **Rationale**, **Alternatives considered**.

## 2026-11-21 — Spell engine: casting model (protocol v4)

**Decision:** Abilities (`abilities.json`) and spells (`spells.json`) share one definition type and one effect resolver (`zone_spells.cpp`); spells are the ones learned from scrolls and cast from gems.
- **Cast state machine** (`spells.hpp`, pure): Idle → Casting (on `CastSpell`) → Idle. The cast completes when its time elapses, or is interrupted or cancelled (`StopCast`, Esc in the client).
- **Costs and timers:** mana is checked when the cast starts and paid when it lands, so an interrupted spell costs nothing. The recast timer and the 1.5 s global cooldown start when it lands.
- **Interrupts:**
  - moving more than 0.5 m;
  - being stunned, mesmerized, or feared;
  - a failed concentration check whenever damage lands (see "Resists and concentration").
- **No melee while casting:** auto-attack swings pause while a spell is being cast.
- **Range and line of sight:** checked when the cast starts, then again when it lands with 10% slack, so a target that drifts slightly out of range still gets hit. Line of sight is tested against zone `obstacles` (vertical cylinders). The test zone has none yet; zone geometry arrives with Phase 6.
- **Wire messages:**
  - client → server: `CastSpell`, `StopCast`, `MemorizeSpell`, `ScribeSpell`, `TrainerOpen`, `TrainerBuy`, `RezResponse`;
  - server → client: `SpellBook`, `GemBar`, `CastStart`/`CastEnd` (sent to everyone within the event radius), `BuffList`, `TrainerWindow`, `RezOffer`.
  - Existing messages gained fields: `CombatEvent.school` (the resist type, which colors log lines and particles), `PlayerStats.run_speed` (so prediction follows snares), and `ItemStack.kind` (the pack offers Scribe on scrolls).

**Rationale:** One resolver means a new spell is a JSON row, never code. Paying at completion and the interrupt rules follow the genre.
**Alternatives considered:**
- Paying mana up front. Rejected: an interruption would then cost as much as a completed cast.
- Separate code paths for abilities and spells. Rejected: Phase 4's hybrids need both.

## 2026-11-21 — Spell effects, buffs, and stacking

**Decision:**
- **Instant effects:** damage, heal, heal_pct, stun, threat, taunt, area_threat, dispel, resurrect, teleport, restore_mana_pct, restore_endurance_pct.
- **Lasting effects:** buff, debuff, slow, snare, root, mez, fear, dot, hot. All lasting lines of one spell from one caster become **one** effect on the target. It holds stat modifiers, control flags, and a periodic amount, and occupies one slot.
- **Slots:** 15 beneficial (buff) and 10 detrimental (debuff) per target, from `rules.spells`.
- **Stacking** (pure `decide_stacking`, tested):
  - Within a `stack_group` (default: the spell's id), an equal or higher `rank` replaces and refreshes the existing effect; a lower rank is blocked, and the caster is told so.
  - Detrimental effects that tick (DoTs) stack **per caster**, so two Wizards' burns both run. Everything else overwrites regardless of caster.
  - Beneficial and detrimental effects never collide.
- **Ticks:** DoTs and HoTs tick every `tick` seconds (default 3 s), the last tick landing on expiry. A DoT ends if its caster leaves the zone.
- **Crowd control:**
  - Mez stops a mob completely (no actions, no aggro scans) and breaks on any damage.
  - Root stops movement but not swings. Each direct hit has a 5% chance to break it. A rooted mob does not leash for being kited.
  - Snare and slow are percentage modifiers to movement speed and attack delay. Slow 50% doubles the delay.
  - Fear makes a mob run from its caster and stop attacking.
  - Mez, stun, root and fear honor a per-effect `max_target_level`; above it the target is immune.
- **Effects on players:** a snare changes the player's speed (sent in `PlayerStats`). Root, mez and fear hold the player in place; fear-running on players waits for pathing. Mez breaks on damage for players too.
- **Dispel:** on an enemy it strips beneficial effects; on a friend (a cure) it strips detrimental ones, newest first.
- **Death:** clears every effect.
- **Polarity rules:** content validation rejects beneficial spells with harmful effects and the reverse.
- **Test hook:** `Zone::force_effect` places a spell's lasting part on any player or mob as if cast by a nameless caster. Tests use it, and it is the path mob spells and GM commands will use.

**Rationale:** One effect per spell matches how players read the buff tray, and slot counts stay meaningful.
**Alternatives considered:** One effect per stat line (what Phase 2 did). Replaced: a two-line buff would cost two slots.

## 2026-11-21 — Resists and concentration

**Decision:**
- **Resist chance:** `clamp(R / (R + 100 + 5 × caster level) + 0.03 × (target level − caster level), 2%, 95%)`, where `R` is the target's resist for the spell's type plus its debuffs, plus the spell's `resist_adjust`. Type `none` is never resisted.
- **Partial resists:** a damage spell that lands is partially resisted with the same chance, keeping 25–100% of its damage.
- **Mob resists:** default to `5 + 0.5 × level`. Body types override some:
  - undead: high poison and disease, moderate cold;
  - constructs: high poison and disease;
  - fire elementals: fire 150, cold 0;
  - plants: fire 0;
  - dragons: fire 80.
- **Concentration** (keeping a cast through a hit): `clamp(0.5 + 0.45 × channeling / (channeling + 100) − 0.6 × damage / max HP, 5%, 97%)`. The `channeling` skill improves with each check.
- **Meditate:** a new skill that raises sitting mana regen by up to 50%.

**Rationale:** An even-level mob resists 5–8% of spells, rising about 3% per level above the caster. Low-level casters are interrupted often in melee, as in the genre, and channeling fixes that with practice.

## 2026-11-21 — Healer aggro

**Decision:** Healing a player gives the healer threat on every living mob that already has that player on its threat list **and** is within `heal_aggro_radius` (60 m) of the healer.
- The amount is half the healing actually done, split evenly between those mobs.
- Buffing someone who is in a fight adds `2 × spell level` threat per such mob.
- Overhealing (no HP restored) and mobs not yet in the fight generate nothing.
- HoT ticks count as heals.

**Rationale:** Healers draw aggro in a way the tank can manage, and healing never pulls a whole camp.

## 2026-11-21 — Learning spells: scrolls, spellbook, gems, trainers

**Decision:**
- **Scrolls:** each spell has a scroll item ("Spell: Name", kind `scroll`).
- **Scribing:** consumes the scroll. It needs the spell's class and level, not already knowing it, and room in the 120-spell book.
- **Gems:**
  - gems open at levels 1, 1, 4, 9, 16, 25, 35 and 45 (2 at start, 8 at 45);
  - a spell sits in one gem at a time;
  - memorizing is refused in combat or while casting, and the gem is not castable for 2 s;
  - losing a level can close gems; their spells stay in the book.
- **Trainers:** NPCs (`npcs.json`, entity kind 3) that sell every scroll of their class at `5 + 2 × level²` copper. You must be within 15 m, and you can buy a scroll above your level but not scribe it.
- **New casters:** start with two level-1 spells scribed and memorized (`classes.json starter_spells`) and 25 copper (`rules.starting_coin`). The third level-1 spell, Frost Needle for Wizards and Holy Rebuke for Clerics, is bought from the trainer, so the very first session goes through the whole path: buy, scribe, memorize.
- **Hotbar:** slots can hold `gem:N`. A class with no abilities of its own gets a default bar of auto-attack plus gems 1-8 on keys 1-9.

**Rationale:** The classic acquisition loop with the fewest moving parts. Trainers selling every rank keeps every spell reachable at its level until vendors and loot tables arrive in Phase 5. Content validation enforces reachability, as the Phase 3 scope requires.
**Alternatives considered:** Gating high ranks behind loot. Deferred to Phase 5/6 content.

## 2026-11-21 — Resurrection

**Decision:** A resurrection spell targets a player corpse whose owner is in the zone (not your own).
- **The offer:** when the spell lands, the owner gets a `RezOffer` that is valid for 60 s, which they accept or decline.
- **Accepting:**
  - restores `pct`% of the XP that death cost (50%, 75% or 96% by rank);
  - revives the owner at the corpse with 20% health, mana and endurance, or teleports them there if they had already respawned;
  - leaves the gear on the corpse to loot;
  - saves the corpse and the character in one transaction.
- **Once per corpse:** a corpse can be resurrected once (`rezzed`). After a restart its `xp_lost` is 0, so a second resurrection restores nothing.

**Rationale:** Classic behavior, with no way to farm XP from one corpse.

## 2026-11-21 — Wizard travel and gather mana

**Decision:**
- `teleport` effects go to `safe_spot` (an evacuate: everything chasing the caster forgets them), `bind`, or a city.
- A teleport is refused at the start of the cast unless its destination is in the caster's zone. Real zone-to-zone portals arrive with zone lines in Phase 6.
- Group-target spells (Wind Retreat) land on the caster only until groups exist (Phase 7).
- Gather Aether restores 60% of mana (20-minute recast); Deep Aether restores 100% (15 minutes).

## 2026-11-21 — Phase 3 spell content and balance

**Decision:** `tools/gen_spells.py` writes 30 Cleric and 30 Wizard spells, their scrolls, and the two trainers; the output is committed.
- **Sizing:** spell power is derived from the server's own curves. A rank learned at level L deals about 38% of an even-con mob's HP (Wizard fire) or heals about 45% of a Warrior's HP at L, then grows per level until it caps near the next rank. Mana cost comes from a damage-per-mana target of about 1.3 + 0.01 × level.
- **Wizard lines:** fire (7 ranks), cold + snare (4), arcane (3, slow but efficient), point-blank AE (3), targeted AE, root (2), a fire DoT, a resist/armor debuff, self wards (3), gate, evacuate, a city translocation, and gather mana (2).
- **Cleric lines:** heals (7), HoTs (3), armor/HP blessings (5), undead nukes (4, undead only), smites (3), stuns with level caps (3), resurrection (3), fear vs undead, and a cure.
- **Balance check** (`[spells][balance]`): a scripted, skill-trained, ungeared caster faces an even-con mob at levels 1, 5, 10, 15, 20, 30, 40, 50 and 60. Wizards win by nuking; Clerics by smiting, meleeing, and healing themselves. It must win at least 75% of fights; today both win every fight.

**Rationale:** Generated tiers keep 60 spells consistent and retunable from one place; hand edits stay possible in the JSON.

## 2026-11-21 — Content validation for spells

**Decision:** Both the server and `tools/validate_content.py` reject:
- a spell that no starter grant, trainer, or loot drop teaches;
- a scroll that teaches an unknown spell, or a trainer selling to a class it does not teach;
- polarity violations (see "Spell effects, buffs, and stacking");
- a corpse target without a resurrect effect;
- a teleport to an unknown destination;
- bad gem-unlock tables;
- **level bands:** any class with abilities that lacks one in each band (1-15, 15-30, 30-45, 45-60). The 11 classes with no abilities yet are skipped until Phase 4 (the Python validator lists them).

## 2026-11-21 — Character schema v3 and persisted timers

**Decision:** Characters gain `spellbook`, `gems`, `buffs` and `timers` (JSON columns, schema v3, migrated in place).
- **Buffs** are saved with their caster level and time left, and rebuilt from the definition on login.
- **Timers:** every ability and spell recast is saved with its time left. This closes the Phase 2 gap where long cooldowns reset on relog.

## 2026-11-21 — Tick rounding

**Decision:** Seconds-to-ticks conversion subtracts a tiny epsilon before rounding up, so 1.95 s at 20 Hz is 39 ticks, not 40, despite float error.

## 2026-11-21 — Client spell UI

**Decision:**
- **Layout:**
  - the gem bar (8 round gems) sits above the hotbar, with the cast bar above it;
  - your effects show as a tray at top right, and your target's under the target frame (blue rim = buff, red = debuff, blinking for the last 10 s, tooltips with caster and description);
  - the spellbook (B) lists spells with their numbers.
- **Memorizing:** drag a spell onto a gem, or press Mem for the first free gem. Right-click forgets a gem.
- **Keys:** T opens a targeted trainer; P shows the group stub; Esc stops a cast.
- **Spell effects:** simple particle placeholders: a ring of motes while casting, and a burst in the school's color when a spell lands.

**Rationale:** Placeholder visuals that read clearly, and every action works by mouse or key.

## 2026-11-20 — Snapshot entity selection

**Decision:** Each player keeps the tick at which each visible entity's state was last sent to it. An entity goes into the next snapshot when:
- it changed in the last tick (it is sent twice, because the channel is unreliable),
- it changed since its last send, or
- it has not been sent for a second (the heartbeat).

Corpses are static and only need their spawn message. If more than 256 entities are due, the longest-waiting go first.
**Rationale:** Idle mobs no longer cost bandwidth every tick, and no entity can be starved by busier neighbors (a CodeRabbit finding).

## 2026-11-20 — Phase 2 combat model (protocol v3)

**Decision:** Tab-target combat runs entirely on the server in the zone tick.
- Clients send intents: `SetTarget`, `AutoAttack`, `UseAbility`, `RespawnRequest`, `LootOpen/Take/Close`, `SetSitting`.
- Failures are answered, not punished. `AbilityResult` carries the reason and the time left on cooldowns.
- Only intents sent outside the world count as violations. A per-session action limiter (10/s, burst 20) silently drops spam.
- The server reports outcomes as `CombatEvent`s (source, target, kind, amount, ability name) to every player within `event_radius`.
- The client formats the combat log and floating text from those events. The server only sends combat-log text for things events don't cover: XP gains and range warnings.
- `PlayerStats` goes on the unreliable channel. It is sent on change at most every other tick, plus a 1 s heartbeat.
- `EntityState` gains `hp_pct` and `flags`. See "Snapshot entity selection" for which entities go out each tick.

**Rationale:** Keeps every rule server-side and the wire small, and the combat log becomes a pure presentation concern.
**Alternatives considered:** Server-formatted log lines. Rejected: more bandwidth, and no structured data for floating text.

## 2026-11-20 — Combat math, mob scaling, and the balance target

**Decision:** All formulas live in `server/src/game/rules.cpp` as pure functions, and every constant is in `content/rules.json`.
- **Hit and avoidance:**
  - Hit chance: base 72%, ±0.15% per skill point of offense versus defense, ±1.5% per level, clamped to 5–95%.
  - Riposte, parry and dodge are checked in that order after a hit roll. Each chance is `skill / (10 × (5 × attacker level + 50))`, capped at 25%; riposte is 0.6× that.
- **Damage:**
  - Armor mitigation is `AC / (AC + 50 + 25 × attacker level)`, capped at 75%.
  - Crits do double damage.
  - Double attack chance is `skill / 500`.
- **Mobs:** HP and max hit are `base + per_level × L + quad × L²`, scaled by per-mob multipliers.
- **Placeholder weapon:** players use a class weapon (base + per-level damage, fixed delay) until items arrive in Phase 5.
- **Balance target:** a Monte Carlo test (`[balance]`) checks a skill-capped, ungeared Warrior against a generic even-con mob. It must win at least 80% of fights, each lasting 5–90 s, at levels 1–60. The current tuning wins nearly every fight in 11–30 s with 60–75% HP left.

**Rationale:** Classic-feeling, tunable, deterministic under a seeded RNG (xoshiro256**). The first tuning failed the balance test from level 20 up: mob damage grew quadratically while player damage grew linearly. The fix was a flatter mob damage curve and about 2.3× weapon damage per level.
**Alternatives considered:** EverQuest's exact tables. Not reproducible from public knowledge, and not needed.

## 2026-11-20 — XP curve and consider colors

**Decision:**
- **XP curve:** `xp_to_next(L) = 10 × L² × 1.0718^(L-1)`. Kill XP is `1.5 × mob_level² × con multiplier`: grey 0, green 0.5, blue 0.85, white 1, yellow 1.15, red 1.3.
- **Pace:** because 1.0718¹⁰ ≈ 2, the kills (and so the time) needed for levels 50→60 roughly equal those for 1→50. A test checks the ratio is between 0.8 and 1.25, and that level 60 is reachable in a finite number of kills.
- **Con colors:**
  - Red at +3 levels or more, yellow at +1/+2, white at even.
  - Below the player's level: blue within `1 + L/7` levels, green within `1 + L/6` more, then grey.
- **Groups:** `split_group_xp` (shares by level, +10% per extra member) is the group hook. Every kill passes a single member until groups arrive in Phase 7.

**Rationale:** Matches the CLAUDE.md rule that levels 50→60 take about as long as 1→50, with a level 2 in about five even-con kills.

## 2026-11-20 — Mob AI and threat

**Decision:**
- **AI states:** a pure state machine (`mob_ai.hpp`): idle, wander, chase, aggro (melee), flee, leash, dead, respawn. The zone applies each state's movement and attacks.
- **Aggro and assist:**
  - Mobs aggro on living, non-grey players within `aggro_radius`, scanning every 4 ticks with a staggered start.
  - Same-faction mobs with `assist` join within their `assist_radius`.
- **Leashing:** mobs leash if they get more than `leash_radius` from home, lose every target, or go 30 s without reaching melee range.
  - A leashing mob is immune ("evades") and forgets its threat.
  - It heals to full when it gets home.
- **Fleeing:** happens once per fight, below `flee_hp_pct`, for 6 s.
- **Threat:**
  - Damage adds threat equal to the damage, and abilities can add bonus threat.
  - A taunt sets the taunter to the current top plus 1.
  - Healing adds half the healed amount, split across the mobs already fighting the healed player.
  - The mob attacks the top of the list.
- **Pathing:** none yet. Zones are flat and mobs move in straight lines.

**Rationale:** Deterministic and unit-tested. Evade-on-leash prevents kiting exploits. Real pathing arrives with zone geometry in Phase 6.
**Alternatives considered:** EverQuest's no-leash behavior. Rejected: it encourages training mobs onto other players.

## 2026-11-20 — Warrior abilities, skills, and passives

**Decision:**
- **Abilities:** 16 Warrior abilities in `content/abilities.json`: 12 active and 4 passive, levels 1–56, all with original names.
  - Taunts: Iron Challenge and Warlord's Challenge.
  - Strikes: Rising Kick, Cleaving Blow and Earthsplitter.
  - Pommel Crack stuns, and fails on mobs more than 2 levels above you or immune to stuns.
  - Rallying Cry and Stalwart Resolve heal.
  - Disciplines: Bulwark Stance and Unbroken Wall (defensive), Battle Fury (offensive).
  - Provoking Roar adds threat to every engaged mob nearby.
- **Effects:** abilities are built from a small effect vocabulary: damage, stun, threat, taunt, buff, heal_pct, restore_endurance_pct, area_threat. Phase 3's spell engine extends it.
- **Timers:** a 1.5 s global cooldown applies unless an ability sets `"gcd": false` (the disciplines). Facing is not checked yet.
- **Skills:**
  - Caps are `min(max, per_level × level + 5)` from the class's unlock level. `skills.json` holds defaults and `classes.json` holds per-class overrides.
  - Skills improve through use (offense per swing; defense and avoidance per incoming attack; ability skills per use) with a chance that falls as the skill nears its cap.
  - A newly unlocked skill starts at 1.
- **Passives:** Dodge, Parry, Double Attack and Riposte are skill-driven. Their ability entries exist for display and must match the skill's unlock level.

**Rationale:** Data-driven from the start, so adding classes in Phases 3 and 4 is mostly content.

## 2026-11-20 — Death, corpses, and loot

**Decision:**
- **On death:**
  - The player loses `10% × xp_to_next(level)` of XP (none below level 6). They can drop a level.
  - Their coin and whole pack move to a corpse that persists for 1 hour in the `corpses` table.
  - The corpse is written in the same transaction as the stripped character, so a crash cannot duplicate or lose items.
- **Respawn:**
  - `RespawnRequest` (the death dialog button) revives the player at full health at their bind point, with a hard movement correction.
  - Camping out while dead revives the character at the bind point before saving.
  - Characters are bound at their starting city.
  - Phase 1 characters have no bind yet and fall back to their city.
  - A bind in another zone uses this zone's safe spot until zone lines exist in Phase 6.
- **Corpse rules:**
  - Only the owner can loot a player corpse, and an emptied one stays until it decays (a resurrection target in Phase 3).
  - Mob corpses roll coin plus up to `max_drops` items from their loot table.
  - Mob corpse loot goes to the player who did the most damage and is still present.
  - An emptied mob corpse vanishes when the window closes. Unlooted mob corpses decay after 5 minutes, empty ones after 30 s.
- **Looting:**
  - Coin is taken automatically when a corpse is opened.
  - Each loot step is saved immediately; player-corpse loot uses one transaction for the corpse and the looter.
  - Only one player can have a corpse open at a time.
  - The window closes if you walk more than 1.5× the loot range away.
- **Corpse ids:** player corpse ids are random 63-bit values chosen on the game thread, so later updates need no database round trip.

**Rationale:** The CLAUDE.md death rules, plus crash-safe persistence.
**Alternatives considered:** Letting the database assign corpse ids. Rejected: loot could arrive before the insert completes.

## 2026-11-20 — Phase 2 inventory and items are placeholders

**Decision:** Characters get a flat 16-slot pack (stacking by item `stack`) and copper-denominated coin. 10 copper = 1 silver, 10 silver = 1 gold, 10 gold = 1 platinum.
`items.json` has 26 loot and consumable placeholders. There is no equipment, and consumables can't be used yet.
**Rationale:** Looting and corpse recovery need somewhere to put things. Phase 5 replaces this with equipment slots, 8 general slots holding bags, and the bank.

## 2026-11-20 — Test zone layout for Phase 2

**Decision:**
- The Proving Flats grows to 2 × 2 km.
- 25 placeholder mob types (levels 1–60) sit in rings by level band around the safe centre, spread on a golden-angle spiral. Level 1 is about 50 m out; Grimjaw the Ancient (60) is at about 940 m.
- The data comes from `tools/gen_phase2_content.py` and is committed as plain JSON.
- Rings of short posts every 100 m mark rising danger.

**Rationale:** One zone (zone lines are Phase 6) covering the whole level range, reachable on foot, and readable at a glance.

## 2026-11-20 — Client combat UI

**Decision:**
- **Code-built HUD** (`ui/hud.gd`, presentation only):
  - Player frame and target frame, with the border and name in the con color.
  - XP bar and a 10-slot hotbar showing cooldown and GCD sweeps.
  - Ability window (K), pack (I), loot window, and death dialog.
- **Hotbar:** drag-and-drop uses Godot's `_get_drag_data`/`_drop_data`. Slot 1 defaults to Attack; layouts persist per character in `user://hotbar_<name>.cfg`.
- **Targeting:** Tab and Shift+Tab cycle living mobs by distance. Clicking picks in screen space, with no physics shapes. F1 targets yourself.
- **Keys:** R attack, 1–0 hotbar, L loot, C consider, X sit.
- **Slash commands:** /sit, /stand, /loc, /con, /attack, /target.
- **Chat:** the window gains a Combat tab.
- **Visuals:** mobs and corpses are simple capsules by model family.
- **Autotest:** `--fight N` and `--die-at X,Z` drive the same UI paths for end-to-end checks.

**Rationale:** Keeps Phase 1's UI-in-code approach, and makes the full combat loop scriptable in CI.

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
