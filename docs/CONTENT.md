# How to add content

All game data lives in JSON under `content/`. The server loads and validates it at startup (and fails
fast, naming the file and the entry), so adding spells, items, mobs, zones or quests never needs code
changes. A few rules:

- **Names are ours.** Every zone, NPC, item, spell and lore name is original; never reuse names from
  other games.
- **Some files are generated.** The big tables (spells, abilities, items, loot, mobs, zones, quests)
  are written by deterministic Python generators in `tools/`. CI runs each generator with `--check`
  and fails if the committed file differs from what the generator would write. So either change the
  generator's spec and re-run it (preferred: the numbers stay on the curves), or add hand-made entries
  where the generator keeps them (noted below).
- **Check before you commit:**

```bash
python3 tools/validate_content.py              # schemas, ID references, level bands, quests, loot, vendors
python3 tools/sync_client_content.py           # refresh the client's display copy (client/data/)
ctest --preset release                         # the server's content tests load everything for real
build/release/tools/balance_sim/emberfall-balance-sim --progression --step 3   # for anything that changes power
```

## Where things live

| file | what | written by |
|---|---|---|
| `rules.json` | every formula's constants: XP curve, combat, regen, mob scaling, economy, spells | hand |
| `races.json`, `classes.json`, `cities.json` | races, the 14 classes (combat curves, skills, kits), starting cities | hand |
| `spells.json`, `abilities.json`, `pets.json` | spells and songs; abilities, disciplines and passives; pet templates | `gen_spells.py` (Cleric, Wizard), `gen_classes.py` (the rest) |
| `items.json`, `item_effects.json`, `loot.json` | items, their procs and clicks, loot tables | `gen_items.py` (scrolls and summoned items: `gen_classes.py`) |
| `mobs.json`, `mob_spells.json` | mob types and the spells their specials cast | `gen_mobs.py` (the 25 test-zone mobs are kept as they are) |
| `zones/*.json`, `travel.json` | zones (format in [ZONES.md](ZONES.md)), ferries, portals and teleport stones | `gen_world.py` (hubs: Godot scenes, see ZONES.md) |
| `npcs.json` | vendors, trainers, bankers, quest givers, guards | several generators, each owning its own NPCs |
| `quests.json` | quests and the class epic chains | `gen_quests.py` |
| `crafts.json` | tradeskills, their rules and every recipe (Phase 13) | `gen_crafts.py` (its materials, drops and supplier stock go into items.json, loot.json and npcs.json through `gen_items.py`; its stations through `gen_world.py`) |
| `factions.json`, `social.json`, `skills.json`, `models.json`, `appearance.json` | factions; chat, groups, raids and guilds; skills; models (see [MODELS.md](MODELS.md)) | hand |

Regenerate in dependency order after changing a spec: `gen_classes.py` → `gen_mobs.py` →
`gen_world.py` → `gen_quests.py` → `gen_items.py` → `gen_crafts.py` → `gen_world.py` →
`sync_client_content.py`.

## A spell

A spell entry (from `content/spells.json`):

```json
{"id": "ember_dart", "name": "Ember Dart", "class": "wizard", "level": 1, "type": "nuke",
 "resource": "mana", "cost": 7, "cast_time": 1.75, "cooldown": 0, "gcd": true, "range": 60,
 "target": "enemy", "resist": "fire", "stack_group": "wizard_fire", "rank": 1,
 "effects": [{"type": "damage", "base": 5.7, "per_level": 3.11, "max": 34}],
 "description": "Hurl searing fire at your target. Rank 1 of the fire line."}
```

- `type`: nuke, heal, dot, hot, buff, debuff, cc, pet, utility, passive or discipline.
- `target`: enemy, self, friendly, group, area, corpse or pet. `resist`: none, magic, fire, cold, poison, disease.
- `effects`: what it does. Amounts scale as `base + per_level x caster level`, capped at `max`; lasting
  effects take `duration` (and `tick` for DoTs/HoTs). Effect types include damage, heal, dot, hot, buff
  (with `stat`), slow, snare, root, mez, stun, fear, charm, lifetap, drain, dispel, summon_pet, teleport,
  resurrect and more; `validate_content.py` lists every one it accepts.
- `stack_group` + `rank`: the stacking rules. A higher rank in the same group replaces a lower one;
  different groups stack (15 buff slots).
- Bard songs set `"song": true`; they pulse every `rules.songs.pulse_seconds` while sung.

**To add one**, add a line to the class's template in `tools/gen_classes.py` (or `gen_spells.py` for
Cleric and Wizard lines): the generator sizes the numbers against an even-level mob at the learn level,
writes the spell, its scroll item, and puts the scroll on that class's trainer. Re-run it and the
item generator. A spell you hand-write into `spells.json` is fine for testing, but the next run of the
generator will flag it.

## An item

```json
{"id": "shortsword_t1", "name": "Rusty Shortsword", "kind": "weapon", "stack": 1, "value": 8,
 "rarity": "common", "slots": ["primary", "secondary"], "weapon": "1h_slash", "damage": 7, "delay": 2.6,
 "req_level": 1, "visual": "sword_basic", "description": "A one-handed blade (level 1)."}
```

Armor carries `ac`, `hp`, `mana`, stats (`str`, `sta`, ...), `resists`, `haste`; anything can carry a
`proc` or `click` (an `item_effects.json` id), `classes`/`races` restrictions, and the `no_trade`,
`lore` and `magic` flags. `visual` picks the model the client draws (see MODELS.md).

**To add gear**, extend the tables in `tools/gen_items.py` (tiers, kinds, named dungeon items):
it derives stats from the tier formulas in DECISIONS.md and puts the item in level-appropriate loot
pools and vendors. Quest rewards and epic weapons are defined next to their quests in
`tools/gen_quests.py`'s epic table and created by `gen_items.py`.

## A mob

```json
{"id": "ember_rat", "name": "an ember rat", "level": 1, "model": "rat", "scale": 0.5,
 "hp_mult": 0.8, "damage_mult": 0.8, "attack_delay": 2.4, "run_speed": 5.0, "aggro_radius": 0,
 "assist": false, "faction": "vermin", "wander_radius": 8, "leash_radius": 60,
 "respawn_seconds": 45, "loot": "ember_rat_loot", "body": "animal"}
```

Health and damage come from `rules.mob_scaling` at the mob's level, times `hp_mult`/`damage_mult`.
Kill experience grows with `hp_mult` too (`rules.xp.toughness_*`), so a tough dungeon mob is worth more.
`specials` add procs, casts (from `mob_spells.json`), enrage, rampage and summons; `resists` override
the level defaults. A `support` special (`every`, `range`, optional `below` and `message`) casts a
friendly mob spell (`"target": "friendly"`, only heal, hot and buff effects) on the mob or an ally of
its faction in the fight: heals go to the most hurt ally under `below` of its health, buffs to the
first who lacks them. The other casting specials name enemy mob spells (harmful effects only).

**To add a family** (a new kind of creature with tiers across levels), add it to `FAMILIES` in
`tools/gen_mobs.py` with its biomes; `gen_world.py` then places its camps in zones of matching biome
and level band, and `gen_items.py` gives it a loot table. Dungeon trash, named rares and bosses are in
the generator's dungeon table.

## A zone

Zones are one file each; the format, collision, dungeons and travel are documented in
[ZONES.md](ZONES.md). Outdoor zones and dungeons are specs in `tools/gen_world.py` (bounds, level band,
biome, neighbours, features); hubs are hand-made Godot scenes whose markers (spawns, NPCs, zone lines)
are exported to server collision by `client/tools/export_zones.gd`. After adding a zone, connect it:
a zone line in each neighbour (generated from the neighbour list), and optionally a travel point in
`travel.json`. `validate_content.py` checks every zone line leads somewhere and every zone is reachable.

## A recipe

```json
{"id": "chain_t1_head", "tradeskill": "smithing", "trivial": 15, "result": {"item": "chain_t1_head", "count": 1},
 "ingredients": [{"item": "copper_bar", "count": 2}], "tools": ["smithing_hammer"]}
```

A recipe is combined at a station of its tradeskill (an NPC of role `station` whose `station` is the
tradeskill's). Ingredients are used up, and a failure loses some of them; tools must be in the pack and
are never used up. `trivial` is the skill past which the recipe teaches nothing and nearly always
succeeds. **To add one**, add it to `recipes()` in `tools/gen_crafts.py` (a new material goes in the
tables at the top, with its drops in `DROPS` or a line in `SUPPLIES`). The validator checks that it can
be made from what vendors sell, monsters drop and other recipes make. The combine rules live in
`crafts.json`'s `rules`.

## A quest

```json
{"id": "emberglade_vermin", "name": "Vermin in the Glade", "giver": "captain_brack", "level": 2,
 "zone": "emberglade", "offer": "Rats and beetles are eating the farmers out...",
 "accepted": "Off with you, then.", "progress": "Not yet finished?", "complete": "Good work.",
 "steps": [{"type": "kill", "mobs": ["plague_rat", "sewer_rat", "stonehide_beetle"], "count": 8,
            "text": "Slay 8 in The Emberglade"}],
 "rewards": {"xp": 0.35, "coin": 8, "faction": {"emberhold_watch": 20}}}
```

Step types: `kill` (any of `mobs`, `count` times; a named boss is a kill step of one), `collect`
(items, from trade loot or a quest drop that only falls while the quest needs it), and `talk` /
`deliver` (to an NPC, optionally handing items over). `rewards.xp` is a fraction of a level at the
quest's level (0.35 = a third of a level); rewards can also give `items`, `coin` and `faction`. Chains
list the quests that must be done first in `prereqs`.

**To add one**, write a short spec in `tools/gen_quests.py`'s list (giver, level, zone, step): kill
steps resolve to the mobs of that family that actually spawn in the zone, so quests always point at
real camps. The generator also writes the giver into `npcs.json`. Class epics are a separate table in
the same file.

## Tuning

Power changes (a class's combat curve, spell numbers, `rules.json`) move the balance. Run the
progression simulator before and after, and record the new numbers in DECISIONS.md:

```bash
build/release/tools/balance_sim/emberfall-balance-sim --progression --step 3 --fights 4 --detail
```

CI fails when a class's time from 1 to 60 drifts beyond 1.4x of the median (solo or grouped).
