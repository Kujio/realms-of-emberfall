#!/usr/bin/env python3
"""Mob generator: writes content/mobs.json and content/mob_spells.json.

Mobs come from archetype families, each a list of level tiers (name, level) sharing a model, body,
biomes (where tools/gen_world.py places their camps), faction, behaviour, resists, trade loot and
special abilities. Dungeons add their own trash, named rares and a boss with a mechanic. The 25
Phase 2 mobs (the test zone's camps, which the tests and bots know) are kept as they are.
Every special that casts names a spell written to mob_spells.json, sized from the mob's level by the
server's own mob damage curve. Loot tables (coin, trade loot, the dungeons' named items) are built from
these rows by tools/gen_items.py.

  python3 tools/gen_mobs.py          # write
  python3 tools/gen_mobs.py --check  # exit 1 if a committed file is stale
Afterwards run gen_items.py (loot), gen_world.py (camps) and sync_client_content.py.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from model_picks import MOB_MODELS, apply as apply_models  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"

LEGACY = {"ember_rat", "dusk_beetle", "mangy_cur", "reed_adder", "road_brigand", "grave_rattler", "thornback_boar", "brigand_lookout",
          "ash_wolf", "silkfang_spider", "moss_golem", "cinder_imp", "ridge_ogre", "hollow_knight", "stormcrow", "bog_horror",
          "ironbark_treant", "ashen_cultist", "basalt_drake", "frost_wraith", "crag_troll", "obsidian_sentinel", "deathless_acolyte",
          "emberwyrm_hatchling", "grimjaw"}


def max_hit(level: int) -> float:
    """rules.json mob_scaling: max_hit_base + per_level * L + quad * L^2."""
    return 3.0 + 1.4 * level + 0.006 * level * level


# ------------------------------------------------------------------------------------------- mob spells

SPELLS: dict[str, dict] = {}


def mob_spell(kind: str, level: int) -> str:
    """A spell of this kind sized for a level-`level` caster; returns its id."""
    sid = f"{kind}_{level}"
    if sid in SPELLS:
        return sid
    mh = max_hit(level)
    base = {"id": sid, "class": "mob", "level": 1, "resource": "none", "cost": 0, "cast_time": 0.0, "cooldown": 0.0, "gcd": False,
            "range": 40.0, "target": "enemy", "stack_group": kind}
    t = {
        "venom": ("Venom", "dot", "poison", [{"type": "dot", "base": max(1, round(mh * 0.22)), "duration": 12, "tick": 3}], "Poison burns in the wound."),
        "plague": ("Festering Bite", "dot", "disease", [{"type": "dot", "base": max(1, round(mh * 0.18)), "duration": 15, "tick": 3}], "A sickness spreads from the bite."),
        "scorch": ("Scorch", "nuke", "fire", [{"type": "damage", "base": max(2, round(mh * 0.6))}], "A lick of flame."),
        "firebolt": ("Firebolt", "nuke", "fire", [{"type": "damage", "base": max(3, round(mh * 1.2))}], "A bolt of fire."),
        "frostbite": ("Frostbite", "dot", "cold", [{"type": "dot", "base": max(1, round(mh * 0.2)), "duration": 12, "tick": 3},
                                                  {"type": "snare", "value": 30, "duration": 12}], "Cold bites and slows."),
        "frostbolt": ("Frostbolt", "nuke", "cold", [{"type": "damage", "base": max(3, round(mh * 1.1))}, {"type": "snare", "value": 25, "duration": 8}], "A shard of ice."),
        "spark": ("Static Bolt", "nuke", "magic", [{"type": "damage", "base": max(3, round(mh * 1.0))}], "A crackle of lightning."),
        "web": ("Sticky Web", "cc", "magic", [{"type": "root", "duration": 6}], "Strands of web hold you fast."),
        "roots": ("Grasping Roots", "cc", "magic", [{"type": "root", "duration": 8}], "Roots burst from the ground and hold you."),
        "drain": ("Life Leech", "nuke", "magic", [{"type": "damage", "base": max(3, round(mh * 0.9))}], "Your life is drawn away."),
        "wail": ("Wail of Dread", "cc", "magic", [{"type": "fear", "duration": 4}], "A wail that freezes the heart."),
        "weaken": ("Sapping Curse", "debuff", "magic", [{"type": "debuff", "stat": "melee_damage_pct", "value": -15, "duration": 30}],
                   "Your strength drains away."),
        # Boss area attacks: hit everyone near the target (spread out) or near the boss (step back).
        "venom_spray": ("Venom Spray", "nuke", "poison", [{"type": "damage", "base": max(4, round(mh * 1.1))},
                                                         {"type": "dot", "base": max(1, round(mh * 0.15)), "duration": 9, "tick": 3}], "A spray of venom."),
        "bone_storm": ("Bone Storm", "nuke", "magic", [{"type": "damage", "base": max(4, round(mh * 1.0))}], "Splinters of bone whirl around."),
        "slag_burst": ("Slag Burst", "nuke", "fire", [{"type": "damage", "base": max(4, round(mh * 1.3))}], "Molten slag sprays the ground."),
        "tidal_crash": ("Tidal Crash", "nuke", "cold", [{"type": "damage", "base": max(4, round(mh * 1.1))}, {"type": "snare", "value": 40, "duration": 6}],
                        "A wave of black water crashes down."),
        "chain_lightning": ("Chain Lightning", "nuke", "magic", [{"type": "damage", "base": max(4, round(mh * 1.4))}], "Lightning leaps between foes."),
        "root_quake": ("Root Quake", "cc", "magic", [{"type": "damage", "base": max(3, round(mh * 0.8))}, {"type": "root", "duration": 5}],
                       "The ground heaves with roots."),
        "dread_pulse": ("Dread Pulse", "cc", "magic", [{"type": "damage", "base": max(3, round(mh * 0.7))}, {"type": "fear", "duration": 5}],
                        "A pulse of pure dread."),
        "obsidian_shatter": ("Obsidian Shatter", "nuke", "fire", [{"type": "damage", "base": max(5, round(mh * 1.5))}], "Shards of black glass explode outward."),
        "fire_breath": ("Fire Breath", "nuke", "fire", [{"type": "damage", "base": max(5, round(mh * 1.8))},
                                                       {"type": "dot", "base": max(1, round(mh * 0.2)), "duration": 9, "tick": 3}], "A torrent of dragonfire."),
    }[kind]
    name, typ, resist, effects, desc = t
    if typ == "cc" and kind not in ("root_quake", "dread_pulse"):
        for e in effects:
            e["max_target_level"] = min(65, level + 10)
    SPELLS[sid] = {**base, "name": name, "type": typ, "resist": resist, "effects": effects, "description": desc}
    return sid


# ------------------------------------------------------------------------------------------- families

# Behaviour presets: aggro radius, assist (radius), flee fraction, wander, leash, respawn.
BEHAVIOUR = {
    "timid": (0, 0, 0.15, 12, 70, 60), "beast": (14, 0, 0.12, 15, 90, 90), "pack": (16, 18, 0.12, 12, 90, 90),
    "social": (16, 20, 0.1, 8, 100, 120), "undead": (18, 16, 0.0, 6, 100, 120), "brute": (18, 0, 0.0, 10, 110, 180),
    "construct": (14, 0, 0.0, 4, 100, 180), "guardian": (20, 20, 0.0, 4, 120, 240),
}

# Each family: model, body, biomes, faction, behaviour, scale, hp, damage, delay, speed, resists, trade item, tiers,
# specials(level) -> list. Tiers: (id, name, level).
FAMILIES = [
    {"key": "rat", "model": "rat", "body": "animal", "biomes": ["meadow", "town", "marsh", "crypt"], "faction": "vermin", "beh": "timid",
     "scale": 0.55, "hp": 0.8, "dmg": 0.85, "delay": 2.4, "speed": 5.5, "trade": "rat_whisker",
     "tiers": [("sewer_rat", "a sewer rat", 3), ("plague_rat", "a plague rat", 7), ("giant_cave_rat", "a giant cave rat", 12), ("ash_rat", "an ash rat", 18)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("plague", L), "chance": 0.12}] if L >= 7 else []},
    {"key": "beetle", "model": "beetle", "body": "insect", "biomes": ["meadow", "hills", "badlands", "wastes"], "faction": "vermin", "beh": "timid",
     "scale": 0.7, "hp": 1.1, "dmg": 0.85, "delay": 2.8, "speed": 4.5, "trade": "beetle_carapace", "resists": {"fire": 15},
     "tiers": [("stonehide_beetle", "a stonehide beetle", 6), ("fire_beetle", "a fire beetle", 11), ("shardback_beetle", "a shardback beetle", 17),
               ("obsidian_beetle", "an obsidian beetle", 26), ("ember_scarab", "an ember scarab", 40)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("scorch", L), "chance": 0.12}] if L in (11, 40) else []},
    {"key": "snake", "model": "snake", "body": "animal", "biomes": ["marsh", "meadow", "bog", "shore", "badlands"], "faction": "vermin", "beh": "beast",
     "scale": 0.8, "hp": 0.9, "dmg": 1.0, "delay": 2.2, "speed": 5.0, "trade": "snake_skin", "resists": {"poison": 30},
     "tiers": [("marsh_viper", "a marsh viper", 9), ("bog_serpent", "a bog serpent", 16), ("dune_asp", "a dune asp", 22),
               ("venomfang_cobra", "a venomfang cobra", 30), ("fen_basilisk", "a fen basilisk", 38)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("venom", L), "chance": 0.15}] +
                           ([{"type": "cast", "effect": mob_spell("web", L), "every": 18, "range": 25, "message": "fixes you with a petrifying stare."}] if L >= 38 else [])},
    {"key": "spider", "model": "spider", "body": "insect", "biomes": ["forest", "pinewood", "bog", "crypt", "tundra"], "faction": "vermin", "beh": "beast",
     "scale": 0.9, "hp": 0.95, "dmg": 1.05, "delay": 2.5, "speed": 6.0, "trade": "spider_silk", "resists": {"poison": 25},
     "tiers": [("woodland_spider", "a woodland spider", 10), ("webspinner", "a webspinner", 15), ("cave_weaver", "a cave weaver", 21),
               ("gloom_widow", "a gloom widow", 28), ("frost_spider", "a frost spider", 37), ("shadowsilk_stalker", "a shadowsilk stalker", 46)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("venom", L), "chance": 0.12}] +
                           ([{"type": "cast", "effect": mob_spell("web", L), "every": 20, "range": 30}] if L in (15, 28, 46) else [])},
    {"key": "wolf", "model": "wolf", "body": "animal", "biomes": ["meadow", "forest", "hills", "tundra", "pinewood", "steppe"], "faction": "beasts", "beh": "pack",
     "scale": 0.9, "hp": 1.0, "dmg": 1.0, "delay": 2.2, "speed": 7.0, "trade": "wolf_pelt",
     "tiers": [("grey_wolf", "a grey wolf", 7), ("timber_wolf", "a timber wolf", 13), ("dire_wolf", "a dire wolf", 21), ("plains_stalker", "a plains stalker", 31),
               ("ice_wolf", "an ice wolf", 36), ("frost_worg", "a frost worg", 42), ("stormfang", "a stormfang", 53)],
     "specials": lambda L: ([{"type": "proc", "effect": mob_spell("frostbite", L), "chance": 0.12}] if L in (36, 42) else []) +
                           ([{"type": "enrage", "below": 0.25, "value": 40}] if L >= 21 else [])},
    {"key": "boar", "model": "boar", "body": "animal", "biomes": ["meadow", "forest", "hills", "badlands"], "faction": "beasts", "beh": "beast",
     "scale": 1.0, "hp": 1.15, "dmg": 1.0, "delay": 2.6, "speed": 6.5, "trade": "boar_tusk",
     "tiers": [("mountain_boar", "a mountain boar", 14), ("tusker", "a great tusker", 20), ("ironbristle_boar", "an ironbristle boar", 28)],
     "specials": lambda L: [{"type": "enrage", "below": 0.3, "value": 50}]},
    {"key": "bird", "model": "bird", "body": "animal", "biomes": ["hills", "shore", "peaks", "steppe", "marsh"], "faction": "beasts", "beh": "beast",
     "scale": 0.8, "hp": 0.85, "dmg": 1.05, "delay": 2.0, "speed": 8.0, "trade": "feather_bundle", "resists": {"magic": 10},
     "tiers": [("carrion_crow", "a carrion crow", 6), ("marsh_heron", "a marsh heron", 11), ("cliff_harpy", "a cliff harpy", 36),
               ("thunderhawk", "a thunderhawk", 52)],
     "specials": lambda L: [{"type": "cast", "effect": mob_spell("spark", L), "every": 14, "range": 30}] if L >= 52 else []},
    {"key": "brigand", "model": "brigand", "body": "humanoid", "biomes": ["meadow", "marsh", "shore", "forest", "badlands"], "faction": "brigands", "beh": "social",
     "scale": 1.0, "hp": 1.0, "dmg": 1.0, "delay": 2.6, "speed": 6.5, "trade": "stolen_trinket",
     "tiers": [("cutpurse", "a cutpurse", 8), ("bandit_raider", "a bandit raider", 12), ("marsh_smuggler", "a marsh smuggler", 17),
               ("redrock_outlaw", "a Redrock outlaw", 23), ("steppe_raider", "a steppe raider", 32)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("venom", L), "chance": 0.08}] if L >= 17 else []},
    {"key": "archer", "model": "brigand_archer", "body": "humanoid", "biomes": ["meadow", "forest", "badlands", "steppe"], "faction": "brigands", "beh": "social",
     "scale": 1.0, "hp": 0.9, "dmg": 1.05, "delay": 2.8, "speed": 6.5, "trade": "stolen_trinket",
     "tiers": [("bandit_sharpshooter", "a bandit sharpshooter", 19), ("outlaw_marksman", "an outlaw marksman", 27)],
     "specials": lambda L: []},
    {"key": "skulk", "model": "humanoid", "body": "humanoid", "biomes": ["hills", "meadow", "badlands", "forest"], "faction": "skulks", "beh": "social",
     "scale": 0.7, "hp": 0.9, "dmg": 0.95, "delay": 2.4, "speed": 6.0, "trade": "skulk_charm",
     "tiers": [("skulk_scavenger", "a skulk scavenger", 4), ("skulk_trapper", "a skulk trapper", 10), ("skulk_hexer", "a skulk hexer", 14),
               ("skulk_warlord", "a skulk warlord", 18)],
     "specials": lambda L: ([{"type": "cast", "effect": mob_spell("spark", L), "every": 12, "range": 30}] if L == 14 else []) +
                           ([{"type": "enrage", "below": 0.3, "value": 40}] if L == 18 else [])},
    {"key": "mirefolk", "model": "humanoid", "body": "humanoid", "biomes": ["marsh", "bog", "shore"], "faction": "mirefolk", "beh": "social",
     "scale": 1.05, "hp": 1.05, "dmg": 1.0, "delay": 2.6, "speed": 6.0, "trade": "mirefolk_scale", "resists": {"cold": 15},
     "tiers": [("mirefolk_hunter", "a mirefolk hunter", 10), ("mirefolk_spearman", "a mirefolk spearman", 15), ("mirefolk_tidecaller", "a mirefolk tidecaller", 20),
               ("mirefolk_chieftain", "a mirefolk chieftain", 25)],
     "specials": lambda L: ([{"type": "cast", "effect": mob_spell("frostbolt", L), "every": 12, "range": 35}] if L == 20 else []) +
                           ([{"type": "rampage", "chance": 0.1}] if L == 25 else [])},
    {"key": "cultist", "model": "cultist", "body": "humanoid", "biomes": ["steppe", "wastes", "ashland"], "faction": "cult", "beh": "social",
     "scale": 1.0, "hp": 0.95, "dmg": 1.0, "delay": 2.8, "speed": 6.5, "trade": "cult_sigil", "resists": {"fire": 20},
     "tiers": [("ashen_acolyte", "an ashen acolyte", 30), ("cinder_zealot", "a cinder zealot", 34), ("flamecaller", "a flamecaller", 41),
               ("deathless_adept", "a deathless adept", 49)],
     "specials": lambda L: [{"type": "cast", "effect": mob_spell("firebolt" if L < 49 else "drain", L), "every": 11, "range": 35}]},
    {"key": "skeleton", "model": "skeleton", "body": "undead", "biomes": ["crypt", "bog", "ashland", "steppe"], "faction": "undead", "beh": "undead",
     "scale": 1.0, "hp": 1.0, "dmg": 1.0, "delay": 2.6, "speed": 5.5, "trade": "bone_chips", "resists": {"cold": 25, "poison": 40, "disease": 40},
     "tiers": [("bone_warrior", "a bone warrior", 13), ("crypt_sentry", "a crypt sentry", 19), ("drowned_bones", "drowned bones", 25),
               ("ashbone_legionnaire", "an ashbone legionnaire", 47)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("plague", L), "chance": 0.08}]},
    {"key": "knight", "model": "hollow_knight", "body": "undead", "biomes": ["crypt", "ashland", "steppe"], "faction": "undead", "beh": "undead",
     "scale": 1.05, "hp": 1.2, "dmg": 1.05, "delay": 3.0, "speed": 5.5, "trade": "rusted_insignia", "resists": {"cold": 25, "poison": 40, "disease": 40},
     "tiers": [("barrow_knight", "a barrow knight", 34), ("deathless_knight", "a deathless knight", 52)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("weaken", L), "chance": 0.1}]},
    {"key": "wraith", "model": "wraith", "body": "undead", "biomes": ["bog", "tundra", "ashland", "crypt"], "faction": "undead", "beh": "undead",
     "scale": 1.0, "hp": 0.9, "dmg": 1.0, "delay": 2.6, "speed": 6.5, "trade": "ectoplasm", "resists": {"cold": 40, "poison": 50, "disease": 50, "magic": 15},
     "tiers": [("bog_wisp", "a bog wisp", 22), ("wailing_shade", "a wailing shade", 31), ("frost_specter", "a frost specter", 39),
               ("deathless_wraith", "a deathless wraith", 50)],
     "specials": lambda L: [{"type": "cast", "effect": mob_spell("wail" if L == 31 else ("frostbite" if L == 39 else "drain"), L), "every": 16, "range": 30}]},
    {"key": "golem", "model": "golem", "body": "construct", "biomes": ["hills", "badlands", "wastes", "peaks"], "faction": "elementals", "beh": "construct",
     "scale": 1.2, "hp": 1.3, "dmg": 0.95, "delay": 3.2, "speed": 4.5, "trade": "golem_core", "resists": {"poison": 60, "disease": 60}, "stun_immune": True,
     "tiers": [("clay_golem", "a clay golem", 23), ("ironstone_golem", "an ironstone golem", 35), ("storm_golem", "a storm golem", 57)],
     "specials": lambda L: [{"type": "cast", "effect": mob_spell("spark", L), "every": 15, "range": 25}] if L >= 57 else []},
    {"key": "treant", "model": "treant", "body": "plant", "biomes": ["forest", "pinewood", "bog"], "faction": "elementals", "beh": "construct",
     "scale": 1.0, "hp": 1.25, "dmg": 0.95, "delay": 3.0, "speed": 4.0, "trade": "living_wood", "resists": {"fire": -20, "cold": 20},
     "tiers": [("sapling_guardian", "a sapling guardian", 12), ("hollowpine_elder", "a hollowpine elder", 27), ("rotwood_treant", "a rotwood treant", 30)],
     "specials": lambda L: [{"type": "cast", "effect": mob_spell("roots", L), "every": 18, "range": 25}]},
    {"key": "imp", "model": "imp", "body": "elemental", "biomes": ["wastes", "badlands", "ashland"], "faction": "fire", "beh": "beast",
     "scale": 0.8, "hp": 0.85, "dmg": 1.05, "delay": 2.2, "speed": 7.0, "trade": "brimstone", "resists": {"fire": 60, "cold": -20},
     "tiers": [("flame_imp", "a flame imp", 27), ("magma_imp", "a magma imp", 43)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("scorch", L), "chance": 0.15}]},
    {"key": "giant", "model": "giant", "body": "giant", "biomes": ["badlands", "hills", "tundra", "ashland", "peaks"], "faction": "giants", "beh": "brute",
     "scale": 1.3, "hp": 1.35, "dmg": 1.1, "delay": 3.4, "speed": 6.0, "trade": "giant_tooth",
     "tiers": [("redrock_ogre", "a Redrock ogre", 25), ("ogre_brute", "an ogre brute", 29), ("frost_giant", "a frost giant", 42), ("ash_troll", "an ash troll", 49),
               ("storm_giant", "a storm giant", 58)],
     "specials": lambda L: [{"type": "enrage", "below": 0.25, "value": 50}, {"type": "rampage", "chance": 0.1}]},
    {"key": "drake", "model": "drake", "body": "dragonkin", "biomes": ["steppe", "wastes", "ashland", "peaks"], "faction": "dragonkin", "beh": "brute",
     "scale": 1.1, "hp": 1.2, "dmg": 1.1, "delay": 2.8, "speed": 7.0, "trade": "drake_scale", "resists": {"fire": 50},
     "tiers": [("steppe_drakeling", "a steppe drakeling", 32), ("cinder_drake", "a cinder drake", 44), ("ash_wyrmling", "an ash wyrmling", 50),
               ("storm_drake", "a storm drake", 59)],
     "specials": lambda L: [{"type": "cast", "effect": mob_spell("firebolt", L), "every": 14, "range": 30}]},
    {"key": "horror", "model": "horror", "body": "abomination", "biomes": ["bog", "marsh"], "faction": "abominations", "beh": "beast",
     "scale": 1.1, "hp": 1.15, "dmg": 1.05, "delay": 2.8, "speed": 5.5, "trade": "horror_ichor", "resists": {"disease": 40},
     "tiers": [("fen_lurker", "a fen lurker", 24), ("mire_horror", "a mire horror", 29)],
     "specials": lambda L: [{"type": "proc", "effect": mob_spell("plague", L), "chance": 0.12}]},
]

# ------------------------------------------------------------------------------------------- factions

CITIES = ["emberhold_watch", "thornwood_wardens", "cragmouth_guard"]
# (id, name, default standing, race adjustments, hits when one of its members dies, description)
FACTIONS = [
    ("emberhold_watch", "The Emberhold Watch", 0, {"highlander": 300, "frostmane": 300, "morrok": 200},
     {"emberhold_watch": -100, "thornwood_wardens": -10, "cragmouth_guard": -10}, "The city guard of Emberhold."),
    ("thornwood_wardens", "The Thornwood Wardens", 0, {"vaelith": 300, "hollowfolk": 300, "duskborn": -250, "morrok": -250},
     {"thornwood_wardens": -100, "emberhold_watch": -10, "cragmouth_guard": -10}, "The briar-wardens who keep Thornwood."),
    ("cragmouth_guard", "The Cragmouth Guard", 0, {"deepkin": 300, "duskborn": 300, "tinkerling": 300},
     {"cragmouth_guard": -100, "emberhold_watch": -10, "thornwood_wardens": -10}, "The garrison of the mountain fortress."),
    ("vermin", "Vermin", -1000, {}, {}, "Rats, beetles and other pests."),
    ("curs", "Mangy Curs", -1000, {}, {}, "Wild dogs."),
    ("beasts", "Beasts of the Wild", -1000, {}, {}, "Wolves, boars, birds and other wild animals."),
    ("brigands", "The Roadside Brigands", -1000, {}, {"emberhold_watch": 3, "thornwood_wardens": 1, "cragmouth_guard": 1},
     "Outlaws who prey on travellers."),
    ("skulks", "The Skulk Tribes", -1000, {}, {"cragmouth_guard": 3, "emberhold_watch": 1}, "Small, cunning raiders of the hills."),
    ("mirefolk", "The Mirefolk", -1000, {}, {"thornwood_wardens": 3}, "Scaled hunters of the fens and their serpent god."),
    ("cult", "The Ashen Cult", -1000, {}, {"emberhold_watch": 2, "thornwood_wardens": 2, "cragmouth_guard": 2}, "Worshippers of fire and of the Deathless."),
    ("undead", "The Restless Dead", -1000, {}, {"emberhold_watch": 1, "thornwood_wardens": 1, "cragmouth_guard": 1}, "Skeletons, wraiths and the deathless."),
    ("elementals", "Wild Elementals", -1000, {}, {}, "Golems, treants and other awakened things."),
    ("fire", "Spawn of the Cinderspire", -1000, {}, {}, "Imps and other creatures of fire."),
    ("giants", "The Giant Clans", -1000, {}, {"cragmouth_guard": 3}, "Ogres, trolls and giants."),
    ("dragonkin", "Dragonkin", -1000, {}, {"emberhold_watch": 2, "thornwood_wardens": 2, "cragmouth_guard": 2}, "Drakes and the Emberwyrm's brood."),
    ("abominations", "Abominations", -1000, {}, {}, "Horrors of the bogs."),
]
# City guards: (id, name, faction, model level) - placed in the hubs by tools/gen_world.py.
GUARDS = [("emberhold_watchman", "an Emberhold watchman", "emberhold_watch"), ("thornwood_warden", "a Thornwood warden", "thornwood_wardens"),
          ("cragmouth_guardsman", "a Cragmouth guardsman", "cragmouth_guard")]


def guard_row(gid: str, name: str, faction: str) -> dict:
    return {"id": gid, "name": name, "level": 55, "model": "humanoid", "scale": 1.05, "hp_mult": 3.0, "damage_mult": 1.4, "attack_delay": 2.6,
            "run_speed": 7.5, "aggro_radius": 30, "assist": True, "assist_radius": 40, "faction": faction, "flee_hp_pct": 0.0, "wander_radius": 0,
            "leash_radius": 60, "respawn_seconds": 300, "stun_immune": True, "charm_immune": True, "loot": f"{gid}_loot", "body": "humanoid",
            "biomes": [], "family": "guard", "guard": True}


# ------------------------------------------------------------------------------------------- dungeons

# Per dungeon: trash (id, name, family, level), named rares (id, name, family, level, replaces trash id), a boss.
# Boss mechanics (CLAUDE.md: adds, an enrage timer, fear, or a spread-out area attack): each boss has at least one.
DUNGEONS = [
    {"id": "rattlebone_warren", "trash": [("warren_rat", "a warren rat", "rat", 3), ("rattling_skeleton", "a rattling skeleton", "skeleton", 6),
                                          ("barrow_ghoul", "a barrow ghoul", "skeleton", 8)],
     "named": [("gnawbone", "Gnawbone", "rat", 8, "warren_rat"), ("old_knucklebones", "Old Knucklebones", "skeleton", 10, "barrow_ghoul")],
     "boss": ("the_rattle_king", "The Rattle King", "skeleton", 11, 1.6, [
         {"type": "adds", "at": [0.66, 0.33], "mob": "rattling_skeleton", "count": 2, "message": "Rise, my subjects! Defend your king!"}])},
    {"id": "mirefang_hollow", "trash": [("hollow_mirefolk", "a hollow mirefolk", "mirefolk", 12), ("hollow_viper", "a hollow viper", "snake", 13),
                                        ("mire_tender", "a mire tender", "mirefolk", 15)],
     "named": [("hissra", "Hissra the Coiled", "snake", 16, "hollow_viper"), ("chief_grubb", "Chief Grubb", "mirefolk", 16, "mire_tender")],
     "boss": ("mirefang", "Mirefang", "horror", 17, 1.8, [
         {"type": "aoe", "effect": "venom_spray", "every": 14, "range": 7, "message": "rears back and sprays venom!"},
         {"type": "proc", "effect": "venom", "chance": 0.15}])},
    {"id": "sunken_bastion", "trash": [("drowned_sailor", "a drowned sailor", "skeleton", 18), ("bastion_crab", "a bastion crab", "beetle", 19),
                                       ("tidecaller_adept", "a tidecaller adept", "mirefolk", 21)],
     "named": [("captain_saltgrave", "Captain Saltgrave", "skeleton", 22, "drowned_sailor")],
     "boss": ("admiral_grell", "Admiral Grell, the Drowned", "knight", 24, 1.4, [
         {"type": "summon", "every": 12, "range": 25, "message": "None leave my ship!"},
         {"type": "adds", "at": [0.5], "mob": "drowned_sailor", "count": 3, "message": "All hands! Repel boarders!"}])},
    {"id": "cinderdeep_mines", "trash": [("mine_ogre", "a mine ogre", "giant", 22), ("slag_beetle", "a slag beetle", "beetle", 21),
                                         ("cinder_sprite", "a cinder sprite", "imp", 24)],
     "named": [("slagjaw", "Slagjaw", "giant", 26, "mine_ogre"), ("the_tunnel_queen", "The Tunnel Queen", "beetle", 25, "slag_beetle")],
     "boss": ("the_cinder_foreman", "The Cinder Foreman", "giant", 27, 1.6, [
         {"type": "enrage_timer", "seconds": 150, "value": 150, "message": "Enough dawdling! Back to work, all of you!"},
         {"type": "rampage", "chance": 0.12},
         {"type": "aoe", "effect": "slag_burst", "every": 18, "range": 8, "center": "self", "message": "slams the ground; molten slag sprays out!"}])},
    {"id": "hollowmere_crypts", "trash": [("crypt_ghoul", "a crypt ghoul", "skeleton", 27), ("hollowmere_shade", "a hollowmere shade", "wraith", 29),
                                          ("tomb_knight", "a tomb knight", "knight", 30)],
     "named": [("brother_ossian", "Brother Ossian", "skeleton", 31, "crypt_ghoul"), ("the_weeping_lady", "The Weeping Lady", "wraith", 32, "hollowmere_shade")],
     "boss": ("the_pale_abbot", "The Pale Abbot", "wraith", 33, 1.5, [
         {"type": "aoe", "effect": "dread_pulse", "every": 22, "range": 12, "center": "self", "message": "raises a skeletal hand. Dread washes over you!"},
         {"type": "adds", "at": [0.6, 0.3], "mob": "crypt_ghoul", "count": 2, "message": "My faithful, rise once more!"}])},
    {"id": "stormspire_ruins", "trash": [("spire_zealot", "a spire zealot", "cultist", 34), ("ruin_golem", "a ruin golem", "golem", 36),
                                         ("spire_drakeling", "a spire drakeling", "drake", 37)],
     "named": [("archon_vessa", "Archon Vessa", "cultist", 39, "spire_zealot"), ("the_rusted_sentinel", "The Rusted Sentinel", "golem", 40, "ruin_golem")],
     "boss": ("the_storm_herald", "The Storm Herald", "cultist", 41, 1.3, [
         {"type": "aoe", "effect": "chain_lightning", "every": 12, "range": 8, "message": "calls down the storm! Lightning leaps from foe to foe!"},
         {"type": "enrage", "below": 0.2, "value": 60, "message": "The storm will not be denied!"}])},
    {"id": "gloomroot_caverns", "trash": [("gloomroot_spider", "a gloomroot spider", "spider", 40), ("cavern_treant", "a cavern treant", "treant", 42),
                                          ("root_horror", "a root horror", "horror", 43)],
     "named": [("silkmother_ysk", "Silkmother Ysk", "spider", 44, "gloomroot_spider"), ("old_gnarl", "Old Gnarl", "treant", 45, "cavern_treant")],
     "boss": ("the_root_mother", "The Root Mother", "treant", 46, 1.7, [
         {"type": "aoe", "effect": "root_quake", "every": 16, "range": 10, "center": "self", "message": "The cavern floor heaves with roots!"},
         {"type": "adds", "at": [0.75, 0.5, 0.25], "mob": "cavern_treant", "count": 1, "message": "Grow, my children!"}])},
    {"id": "deathless_sanctum", "trash": [("sanctum_acolyte", "a sanctum acolyte", "cultist", 47), ("sanctum_knight", "a sanctum knight", "knight", 49),
                                          ("sanctum_wraith", "a sanctum wraith", "wraith", 50)],
     "named": [("confessor_maul", "Confessor Maul", "knight", 52, "sanctum_knight")],
     "boss": ("high_priest_vaurn", "High Priest Vaurn", "cultist", 53, 1.4, [
         {"type": "aoe", "effect": "dread_pulse", "every": 20, "range": 12, "center": "self", "message": "Kneel before the Deathless!"},
         {"type": "cast", "effect": "drain", "every": 10, "range": 35},
         {"type": "adds", "at": [0.5], "mob": "sanctum_wraith", "count": 2, "message": "Servants, to me!"}])},
    {"id": "obsidian_citadel", "raid": True, "trash": [("citadel_sentinel", "a citadel sentinel", "golem", 52), ("obsidian_warden", "an obsidian warden", "golem", 54),
                                                       ("citadel_flamecaller", "a citadel flamecaller", "cultist", 54)],
     "named": [("the_glass_marshal", "The Glass Marshal", "golem", 56, "obsidian_warden")],
     "boss": ("the_obsidian_tyrant", "The Obsidian Tyrant", "golem", 58, 2.6, [
         {"type": "aoe", "effect": "obsidian_shatter", "every": 15, "range": 8, "message": "Shatter!"},
         {"type": "enrage_timer", "seconds": 300, "value": 200, "message": "You have wasted my time. Now you die."},
         {"type": "adds", "at": [0.7, 0.4], "mob": "citadel_sentinel", "count": 3, "message": "Sentinels! Crush them!"},
         {"type": "summon", "every": 15, "range": 30}])},
    {"id": "emberwyrm_lair", "raid": True, "trash": [("lair_drake", "a lair drake", "drake", 55), ("wyrm_cultist", "a wyrm cultist", "cultist", 56),
                                                     ("emberwyrm_brood", "an emberwyrm broodling", "drake", 57)],
     "named": [("scorchwing", "Scorchwing", "drake", 58, "lair_drake")],
     "boss": ("the_emberwyrm", "The Emberwyrm", "drake", 60, 4.0, [
         {"type": "aoe", "effect": "fire_breath", "every": 13, "range": 12, "center": "self", "message": "inhales deeply... and breathes fire!"},
         {"type": "summon", "every": 14, "range": 35, "message": "There is no escape from my lair!"},
         {"type": "enrage_timer", "seconds": 360, "value": 250, "message": "The Emberwyrm's fury knows no bounds!"},
         {"type": "adds", "at": [0.75, 0.5, 0.25], "mob": "emberwyrm_brood", "count": 2, "message": "My brood! Feed!"}])},
]


def row(mid: str, name: str, fam: dict, level: int, **over) -> dict:
    aggro, assist, flee, wander, leash, respawn = BEHAVIOUR[fam["beh"]]
    r = {"id": mid, "name": name, "level": level, "model": fam["model"], "scale": fam["scale"], "hp_mult": fam["hp"], "damage_mult": fam["dmg"],
         "attack_delay": fam["delay"], "run_speed": fam["speed"], "aggro_radius": aggro if level > 2 else 0, "assist": assist > 0,
         "assist_radius": assist, "faction": fam["faction"], "flee_hp_pct": flee, "wander_radius": wander, "leash_radius": leash,
         "respawn_seconds": respawn, "stun_immune": bool(fam.get("stun_immune", False)), "loot": f"{mid}_loot", "body": fam["body"],
         "biomes": fam["biomes"], "family": fam["key"], "trade": fam["trade"]}
    if fam.get("resists"):
        base = 5 + level // 2
        r["resists"] = {k: max(0, base + v) for k, v in fam["resists"].items()}
    specials = fam["specials"](level)
    if specials:
        r["specials"] = specials
    r.update(over)
    return r


def resolve_specials(specials: list[dict], level: int) -> list[dict]:
    """Boss specials name a spell kind; size it for the boss's level."""
    out = []
    for s in specials:
        s = dict(s)
        if "effect" in s:
            s["effect"] = mob_spell(s["effect"], level)
        out.append(s)
    return out


def main() -> int:
    check = "--check" in sys.argv
    old = json.loads((CONTENT / "mobs.json").read_text(encoding="utf-8"))["mobs"]
    legacy = [m for m in old if m["id"] in LEGACY]
    missing = LEGACY - {m["id"] for m in legacy}
    if missing:
        sys.exit(f"gen_mobs: legacy mobs missing from mobs.json: {sorted(missing)}")
    fams = {f["key"]: f for f in FAMILIES}
    rows: list[dict] = []
    for f in FAMILIES:
        for mid, name, lvl in f["tiers"]:
            rows.append(row(mid, name, f, lvl))
    for d in DUNGEONS:
        did = d["id"]
        raid = d.get("raid", False)
        for mid, name, fk, lvl in d["trash"]:
            rows.append(row(mid, name, fams[fk], lvl, dungeon=did, biomes=[], respawn_seconds=900 if raid else 480,
                            hp_mult=round(fams[fk]["hp"] * (2.5 if raid else 1.1), 2), aggro_radius=20, assist=True, assist_radius=22, flee_hp_pct=0.0))
        for mid, name, fk, lvl, rare_of in d["named"]:
            rows.append(row(mid, name, fams[fk], lvl, dungeon=did, biomes=[], named=True, charm_immune=True, respawn_seconds=1800, rare_of=rare_of,
                            hp_mult=round(fams[fk]["hp"] * (4.0 if raid else 1.8), 2), damage_mult=round(fams[fk]["dmg"] * 1.15, 2),
                            scale=round(fams[fk]["scale"] * 1.2, 2), aggro_radius=20, flee_hp_pct=0.0))
        bid, bname, fk, lvl, bscale, bspecials = d["boss"]
        rows.append(row(bid, bname, fams[fk], lvl, dungeon=did, biomes=[], boss=True, named=True, charm_immune=True, stun_immune=True,
                        respawn_seconds=7200 if raid else 2700, hp_mult=30.0 if raid else 7.0, damage_mult=2.2 if raid else 1.5,
                        scale=round(fams[fk]["scale"] * bscale, 2), aggro_radius=22, flee_hp_pct=0.0, wander_radius=0, leash_radius=150,
                        specials=resolve_specials(bspecials, lvl)))
    rows += [guard_row(*g) for g in GUARDS]
    ids = [r["id"] for r in rows] + [m["id"] for m in legacy]
    dupes = {i for i in ids if ids.count(i) > 1}
    if dupes:
        sys.exit(f"gen_mobs: duplicate mob ids {sorted(dupes)}")
    for r in rows:
        if len(r["id"]) > 32 or len(r["name"]) > 48:
            sys.exit(f"gen_mobs: id/name too long: {r['id']} / {r['name']}")
    mobs = apply_models(legacy + sorted(rows, key=lambda r: (r["level"], r["id"])), MOB_MODELS, "name")
    mob_text = render("Mobs (generated by tools/gen_mobs.py from archetype families, dungeon trash, named rares and bosses; the first 25 are the "
                      "Phase 2 test-zone mobs, kept as they were). Stats are rules.mob_scaling times the multipliers; 'specials' are abilities "
                      "and boss mechanics (see server content.hpp MobSpecial); 'biomes' tells tools/gen_world.py where camps of the mob belong; "
                      "'trade' and 'dungeon' tell tools/gen_items.py what the loot table drops.", "mobs", mobs)
    spells = sorted(SPELLS.values(), key=lambda s: s["id"])
    spell_text = render("What mob special abilities cast (generated by tools/gen_mobs.py): procs, spells and area attacks, sized for the "
                        "caster's level from the server's mob damage curve. class is always mob.", "spells", spells)
    fac_rows = [{"id": f, "name": n, "default": d, **({"race_mods": rm} if rm else {}), **({"hits": h} if h else {}), "description": desc}
                for f, n, d, rm, h, desc in FACTIONS]
    used = {m["faction"] for m in mobs}
    unknown = used - {f[0] for f in FACTIONS}
    if unknown:
        sys.exit(f"gen_mobs: mobs use factions with no definition: {sorted(unknown)}")
    fac_text = render("Factions (generated by tools/gen_mobs.py). Standing = default + race_mods[race] + what deeds earned, clamped to "
                      "-2000..2000. Killing a member applies 'hits'. Its members attack on sight at threatening (-750) and below; service "
                      "NPCs of the faction refuse you from dubious (-500). See server game/factions.hpp.", "factions", fac_rows)
    outputs = [(CONTENT / "mobs.json", mob_text), (CONTENT / "mob_spells.json", spell_text), (CONTENT / "factions.json", fac_text)]
    stale = []
    for path, text in outputs:
        if check:
            if path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
    if check and stale:
        print("gen_mobs: stale generated content (run python3 tools/gen_mobs.py):", ", ".join(stale))
        return 1
    if not check:
        print(f"wrote {len(mobs)} mobs ({len(rows)} generated: {sum(1 for r in rows if r.get('boss'))} bosses, "
              f"{sum(1 for r in rows if r.get('named') and not r.get('boss'))} named) and {len(spells)} mob spells")
    return 0


def render(comment: str, key: str, rows: list[dict]) -> str:
    lines = ["{", '  "_comment": ' + json.dumps(comment) + ",", f'  "{key}": [']
    for i, r in enumerate(rows):
        lines.append("    " + json.dumps(r) + ("," if i < len(rows) - 1 else ""))
    lines += ["  ]", "}", ""]
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
