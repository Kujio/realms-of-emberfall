#!/usr/bin/env python3
"""One-shot generator for the Phase 2 placeholder content (items, mobs, loot tables, zone spawns).

The JSON it writes is committed and hand-editable; rerun this only to rebuild the placeholder set.
Usage: python tools/gen_phase2_content.py
"""
from __future__ import annotations

import json
import sys
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from zone_format import format_zone  # noqa: E402
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
ZONE_ID = "proving_flats"  # the zone whose spawns this script owns


def dump(name: str, comment: str, key: str, rows: list[dict]) -> None:
    lines = ["{", '  "_comment": ' + json.dumps(comment) + ",", f'  "{key}": [']
    for i, r in enumerate(rows):
        lines.append("    " + json.dumps(r) + ("," if i < len(rows) - 1 else ""))
    lines += ["  ]", "}", ""]
    (CONTENT / name).write_text("\n".join(lines), encoding="utf-8", newline="\n")


ITEMS = [
    ("rat_whisker", "Rat Whisker", "loot", 20, 1, "common", "A stiff, singed whisker. Some alchemist might want it."),
    ("cracked_carapace", "Cracked Carapace", "loot", 20, 2, "common", "A piece of beetle shell, still warm."),
    ("tattered_pelt", "Tattered Pelt", "loot", 10, 4, "common", "A scruffy hide, good for little."),
    ("snake_fang", "Adder Fang", "loot", 20, 5, "common", "A hollow fang that still drips a little venom."),
    ("bone_chips", "Bone Chips", "loot", 20, 3, "common", "Splinters of old bone."),
    ("brigand_badge", "Brigand Badge", "loot", 20, 8, "common", "A crude tin badge worn by road brigands."),
    ("boar_tusk", "Thornback Tusk", "loot", 10, 12, "common", "A curved tusk ridged with spines."),
    ("wolf_fang", "Ash Wolf Fang", "loot", 20, 15, "common", "A grey fang dusted with soot."),
    ("spider_silk", "Silkfang Silk", "loot", 20, 20, "common", "A strong, sticky strand."),
    ("moss_heart", "Mossy Heartstone", "loot", 10, 40, "uncommon", "A green stone that pulses faintly."),
    ("ember_shard", "Ember Shard", "loot", 20, 45, "common", "A sliver of crystal that never cools."),
    ("ogre_tooth", "Ogre Molar", "loot", 10, 60, "common", "Huge, yellow, and unpleasant."),
    ("hollow_crest", "Tarnished Crest", "loot", 10, 90, "uncommon", "The badge of a knightly order nobody remembers."),
    ("storm_feather", "Storm Feather", "loot", 20, 80, "common", "It crackles when you touch it."),
    ("bog_ichor", "Bog Ichor", "loot", 20, 100, "common", "Foul black sludge in a stoppered vial."),
    ("ironbark_chunk", "Ironbark Chunk", "loot", 10, 150, "common", "Wood as hard as steel."),
    ("cult_sigil", "Ashen Sigil", "loot", 10, 200, "uncommon", "A charred token bearing a spiral mark."),
    ("drake_scale", "Basalt Drake Scale", "loot", 10, 300, "uncommon", "A dark scale that holds heat for hours."),
    ("wraith_essence", "Frozen Essence", "loot", 10, 350, "uncommon", "A shard of cold that never melts."),
    ("troll_hide", "Crag Troll Hide", "loot", 5, 400, "common", "Thick, warty, and still twitching."),
    ("obsidian_core", "Obsidian Core", "loot", 5, 600, "rare", "The glassy heart of a sentinel."),
    ("acolyte_phylactery", "Cracked Phylactery", "loot", 5, 700, "rare", "Something whispers inside."),
    ("wyrm_scale", "Emberwyrm Scale", "loot", 5, 900, "rare", "Red-gold and too hot to hold for long."),
    ("grimjaw_tooth", "Grimjaw's Broken Tooth", "loot", 1, 5000, "epic", "A trophy from the oldest beast on the Flats."),
    ("trail_rations", "Trail Rations", "consumable", 20, 5, "common", "Dry bread and smoked meat. Eating arrives in a later update."),
    ("minor_draught", "Minor Healing Draught", "consumable", 10, 25, "uncommon", "A small red potion. Drinking arrives in a later update."),
]

# id, name, level, model, scale, hp_mult, damage_mult, attack_delay, run_speed, aggro_radius, assist,
# faction, flee_hp_pct, wander_radius, leash_radius, respawn_seconds, stun_immune, signature drop
MOBS = [
    ("ember_rat", "an ember rat", 1, "rat", 0.5, 0.8, 0.8, 2.4, 5.0, 0, False, "vermin", 0.0, 8, 60, 45, False, "rat_whisker"),
    ("dusk_beetle", "a dusk beetle", 2, "beetle", 0.6, 1.0, 0.8, 2.6, 4.0, 0, False, "vermin", 0.0, 6, 60, 45, False, "cracked_carapace"),
    ("mangy_cur", "a mangy cur", 3, "wolf", 0.8, 0.9, 1.0, 2.2, 6.0, 8, True, "curs", 0.15, 10, 70, 60, False, "tattered_pelt"),
    ("reed_adder", "a reed adder", 4, "snake", 0.5, 0.9, 1.0, 2.4, 5.0, 6, False, "vermin", 0.0, 6, 70, 60, False, "snake_fang"),
    ("road_brigand", "a road brigand", 5, "brigand", 1.0, 1.0, 1.0, 2.8, 6.0, 12, True, "brigands", 0.15, 10, 80, 90, False, "brigand_badge"),
    ("grave_rattler", "a grave rattler", 7, "skeleton", 1.0, 1.0, 1.0, 2.8, 5.5, 10, True, "undead", 0.0, 6, 80, 90, False, "bone_chips"),
    ("thornback_boar", "a thornback boar", 9, "boar", 0.9, 1.2, 1.0, 2.6, 6.5, 0, False, "beasts", 0.0, 12, 90, 120, False, "boar_tusk"),
    ("brigand_lookout", "a brigand lookout", 11, "brigand_archer", 1.0, 1.0, 1.05, 2.8, 6.5, 14, True, "brigands", 0.15, 8, 90, 120, False, "brigand_badge"),
    ("ash_wolf", "an ash wolf", 13, "wolf", 1.0, 0.95, 1.0, 2.2, 7.0, 15, True, "beasts", 0.1, 14, 100, 120, False, "wolf_fang"),
    ("silkfang_spider", "a silkfang spider", 15, "spider", 0.9, 1.0, 1.05, 2.5, 6.0, 10, True, "vermin", 0.0, 8, 100, 150, False, "spider_silk"),
    ("moss_golem", "a moss golem", 17, "golem", 1.6, 1.4, 1.1, 3.4, 4.5, 8, False, "elementals", 0.0, 4, 100, 180, True, "moss_heart"),
    ("cinder_imp", "a cinder imp", 20, "imp", 0.6, 0.8, 1.1, 2.0, 7.0, 16, True, "fire", 0.15, 12, 110, 180, False, "ember_shard"),
    ("ridge_ogre", "a ridge ogre", 23, "giant", 1.8, 1.3, 1.2, 3.4, 6.0, 12, False, "giants", 0.0, 10, 110, 240, False, "ogre_tooth"),
    ("hollow_knight", "a hollow knight", 26, "hollow_knight", 1.1, 1.15, 1.1, 3.0, 6.0, 14, True, "undead", 0.0, 6, 110, 240, False, "hollow_crest"),
    ("stormcrow", "a stormcrow", 29, "bird", 0.8, 0.9, 1.1, 2.2, 8.0, 18, True, "beasts", 0.1, 16, 120, 240, False, "storm_feather"),
    ("bog_horror", "a bog horror", 32, "horror", 1.4, 1.2, 1.1, 3.0, 5.0, 12, False, "abominations", 0.0, 6, 120, 300, False, "bog_ichor"),
    ("ironbark_treant", "an ironbark treant", 35, "treant", 2.2, 1.5, 1.15, 3.6, 4.5, 10, True, "elementals", 0.0, 3, 120, 300, True, "ironbark_chunk"),
    ("ashen_cultist", "an ashen cultist", 38, "cultist", 1.0, 1.0, 1.1, 2.8, 6.5, 16, True, "cult", 0.15, 8, 130, 300, False, "cult_sigil"),
    ("basalt_drake", "a basalt drake", 41, "drake", 1.8, 1.3, 1.2, 3.0, 7.0, 18, False, "dragonkin", 0.0, 12, 130, 360, False, "drake_scale"),
    ("frost_wraith", "a frost wraith", 44, "wraith", 1.1, 1.0, 1.15, 2.6, 7.0, 16, True, "undead", 0.0, 10, 130, 360, False, "wraith_essence"),
    ("crag_troll", "a crag troll", 47, "giant", 2.0, 1.4, 1.2, 3.2, 6.5, 14, True, "giants", 0.1, 10, 140, 420, False, "troll_hide"),
    ("obsidian_sentinel", "an obsidian sentinel", 50, "golem", 2.0, 1.6, 1.2, 3.6, 5.0, 12, False, "elementals", 0.0, 0, 140, 480, True, "obsidian_core"),
    ("deathless_acolyte", "a deathless acolyte", 53, "wraith", 1.1, 1.1, 1.2, 2.8, 6.5, 18, True, "undead", 0.0, 8, 140, 480, False, "acolyte_phylactery"),
    ("emberwyrm_hatchling", "an emberwyrm hatchling", 56, "drake", 2.4, 1.5, 1.25, 3.0, 7.0, 20, False, "dragonkin", 0.0, 12, 150, 600, True, "wyrm_scale"),
    ("grimjaw", "Grimjaw the Ancient", 60, "wolf", 2.6, 3.0, 1.3, 2.4, 7.5, 22, False, "beasts", 0.0, 0, 160, 1800, True, "grimjaw_tooth"),
]


# Body type by model (spells such as undead nukes check it) and the resists that go with it.
BODY = {"rat": "animal", "beetle": "insect", "wolf": "animal", "snake": "animal", "humanoid": "humanoid",
        "skeleton": "undead", "boar": "animal", "spider": "insect", "golem": "construct", "imp": "elemental",
        "giant": "giant", "bird": "animal", "horror": "abomination", "treant": "plant", "drake": "dragon", "wraith": "undead",
        # Phase 3.5 model ids (content/models.json) for mob types that got their own model.
        "brigand": "humanoid", "brigand_archer": "humanoid", "cultist": "humanoid", "hollow_knight": "undead"}
BODY_RESISTS = {
    "undead": {"cold": 60, "poison": 150, "disease": 150},
    "construct": {"poison": 200, "disease": 200},
    "elemental": {"fire": 150, "cold": 0},
    "plant": {"fire": 0, "cold": 40},
    "dragon": {"fire": 80},
}


def ring_radius(level: int) -> float:
    """Camps sit in rings by level band, out from the safe centre."""
    if level <= 5:
        return 50 + (level - 1) * 18
    if level <= 15:
        return 150 + (level - 6) * 16
    if level <= 29:
        return 320 + (level - 17) * 14
    if level <= 44:
        return 520 + (level - 32) * 13
    return 720 + (level - 47) * 17


def main() -> None:
    # Spell scrolls belong to tools/gen_spells.py, and placeholder class gear (kind "gear", with item
    # visuals) is hand-authored: keep both. Everything else in items.json is this script's, so an item
    # dropped from ITEMS disappears on the next run.
    existing = json.loads((CONTENT / "items.json").read_text(encoding="utf-8")).get("items", []) if (CONTENT / "items.json").exists() else []
    others = [it for it in existing if it.get("kind") == "gear"]
    scrolls = [it for it in existing if it.get("kind") == "scroll"]
    dump("items.json", "Placeholder items: loot, future consumables, and spell scrolls (tools/gen_spells.py). Full itemization (600+ items, equipment) is Phase 5. value is in copper.",
         "items", [dict(id=i, name=n, kind=k, stack=s, value=v, rarity=r, description=d) for i, n, k, s, v, r, d in ITEMS] + others + scrolls)

    mob_rows, loot_rows, spawns = [], [], []
    for idx, (mid, name, lvl, model, scale, hp, dmg, delay, speed, aggro, assist, fac, flee, wander, leash, respawn,
              stun_imm, drop) in enumerate(MOBS):
        table = f"{mid}_loot"
        mob_rows.append(dict(id=mid, name=name, level=lvl, model=model, scale=scale, hp_mult=hp, damage_mult=dmg,
                             attack_delay=delay, run_speed=speed, aggro_radius=aggro, assist=assist,
                             assist_radius=(15 if assist else 0), faction=fac, flee_hp_pct=flee, wander_radius=wander,
                             leash_radius=leash, respawn_seconds=respawn, stun_immune=stun_imm, loot=table, body=BODY[model]))
        if BODY[model] in BODY_RESISTS:
            mob_rows[-1]["resists"] = BODY_RESISTS[BODY[model]]
        coin_max = int(2 + lvl * lvl * 0.8)
        drops = [dict(item=drop, chance=(1.0 if mid == "grimjaw" else 0.6), min=1, max=(1 if lvl >= 17 else 2))]
        if lvl <= 15:
            drops.append(dict(item="trail_rations", chance=0.1, min=1, max=1))
        drops.append(dict(item="minor_draught", chance=0.05 if lvl < 30 else 0.1, min=1, max=1))
        loot_rows.append(dict(id=table, coin_min=coin_max // 3, coin_max=coin_max, max_drops=2, drops=drops))

        r = ring_radius(lvl)
        ang = idx * 2.399963  # golden angle spreads the camps around the compass
        count = 1 if mid == "grimjaw" else (4 if lvl <= 5 else 3 if lvl <= 29 else 2)
        spawns.append(dict(mob=mid, x=round(r * math.cos(ang), 1), z=round(r * math.sin(ang), 1), count=count,
                           radius=12.0 if count > 1 else 0.0))

    dump("mobs.json", "Phase 2 placeholder mobs across levels 1-60. Stats come from rules.mob_scaling times the multipliers here. "
         "faction groups mobs for assist; real factions arrive in Phase 6.", "mobs", mob_rows)
    dump("loot.json", "Loot tables. Coin is in copper. Each drop rolls independently; at most max_drops items drop.", "loot_tables", loot_rows)

    # Update only the placeholder zone's camps (content/zones/proving_flats.json); keep everything else.
    zpath = CONTENT / "zones" / f"{ZONE_ID}.json"
    zone = json.loads(zpath.read_text(encoding="utf-8"))
    zone["bounds"] = {"min_x": -1000.0, "min_z": -1000.0, "max_x": 1000.0, "max_z": 1000.0}
    zone["spawns"] = spawns
    zpath.write_text(format_zone(zone), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
