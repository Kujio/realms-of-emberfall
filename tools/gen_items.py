#!/usr/bin/env python3
"""Generate Realms of Emberfall's itemization (Phase 5).

Writes (deterministically; commit the output):
  content/items.json         armor, weapons, shields, jewelry, ammo, consumables, bags, named dungeon placeholders,
                             the starter "training" gear and trade loot (spell scrolls and summoned items are kept:
                             tools/gen_classes.py owns them)
  content/item_effects.json  the spell effects items carry: weapon procs, potions, food and drink, bandages, elixirs
  content/loot.json          every mob's loot table: its trade drops plus weighted pools of level-appropriate gear,
                             consumables and spell scrolls
  content/npcs.json          vendors by level band, a tradeskill supplier, a banker and an auctioneer (trainers and
                             lockboxes are kept: tools/gen_classes.py owns them)

Phase 13: the tradeskill materials and tools, their monster drops (pools rolled after a table's own) and the
supplier's stock come from tools/gen_crafts.py; run it after this (it reads items.json for its recipes).

Design (see docs/DECISIONS.md, Phase 5):
  * Armor: cloth/leather/chain/plate x 9 slots x 8 tiers. AC = weight x slot share x (1.5 x level + 5);
    stats from a budget that grows with level and rarity (common pieces have none).
  * Weapons: 16 kinds x 12 tiers. Damage per second at a tier's level follows the old class placeholder weapon
    (4 + 1.15 x (level + 2.5)) / 3 s, scaled by kind (two-handers hit harder) and rarity; the class's weapon_mult
    (classes.json) then scales what each class gets out of it.
  * Value (copper) grows with the square of level; vendors add their markup (rules.json economy).

Usage: python tools/gen_items.py [--check]
"""
from __future__ import annotations

import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parent))
from npc_order import ordered, render as render_npcs  # noqa: E402
from gen_crafts import SUPPLIER_TEMPLATE, SUPPLIES, balance_values, drops_for, material_items  # noqa: E402
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
ZONE = "proving_flats"
MAX_LEVEL = 60

RARITY_STAT = {"common": 0.0, "uncommon": 1.0, "rare": 1.6, "epic": 2.4}
RARITY_VALUE = {"common": 1.0, "uncommon": 2.5, "rare": 6.0, "epic": 15.0}
RARITY_DAMAGE = {"common": 1.0, "uncommon": 1.07, "rare": 1.15, "epic": 1.25}
RARITY_LOOT_WEIGHT = {"common": 10.0, "uncommon": 5.0, "rare": 1.5, "epic": 0.3}

# ------------------------------------------------------------------------------------------- armor

ARMOR_TIERS = [1, 8, 15, 22, 30, 38, 46, 54]
ARMOR_WEIGHT = {"cloth": 0.4, "leather": 0.6, "chain": 0.8, "plate": 1.0}
ARMOR_SLOTS = ["head", "shoulders", "chest", "arms", "wrist", "hands", "waist", "legs", "feet"]
SLOT_SHARE = {"head": 0.10, "shoulders": 0.08, "chest": 0.20, "arms": 0.08, "wrist": 0.05, "hands": 0.08, "waist": 0.06, "legs": 0.15, "feet": 0.10,
              "neck": 0.06, "ear": 0.04, "finger": 0.04, "face": 0.05, "back": 0.08, "shield": 0.15}
MATERIAL = {
    "cloth": ["Homespun", "Linen", "Woolen", "Silkweave", "Spellthread", "Moonweave", "Starweave", "Emberweave"],
    "leather": ["Rawhide", "Tanned", "Studded", "Wyrmhide", "Shadowhide", "Drakehide", "Thornhide", "Emberhide"],
    "chain": ["Rusted Ring", "Ringmail", "Chainmail", "Scaled", "Bronzelink", "Steelwoven", "Mithrite", "Emberlink"],
    "plate": ["Dented", "Iron", "Banded", "Steel", "Tempered", "Runeforged", "Obsidian", "Emberforged"],
}
NOUN = {
    "cloth": {"head": "Hood", "shoulders": "Mantle", "chest": "Robe", "arms": "Sleeves", "wrist": "Wristwraps", "hands": "Gloves", "waist": "Sash",
              "legs": "Leggings", "feet": "Slippers"},
    "leather": {"head": "Cap", "shoulders": "Shoulderpads", "chest": "Jerkin", "arms": "Sleeves", "wrist": "Bracer", "hands": "Gloves", "waist": "Belt",
                "legs": "Leggings", "feet": "Boots"},
    "chain": {"head": "Coif", "shoulders": "Spaulders", "chest": "Hauberk", "arms": "Sleeves", "wrist": "Bracer", "hands": "Gauntlets", "waist": "Girdle",
              "legs": "Chausses", "feet": "Boots"},
    "plate": {"head": "Helm", "shoulders": "Pauldrons", "chest": "Breastplate", "arms": "Vambraces", "wrist": "Bracer", "hands": "Gauntlets",
              "waist": "Girdle", "legs": "Greaves", "feet": "Sabatons"},
}
# How a stat budget splits for each armor weight (point costs: 1 stat = 1, hp/mana = 1 point per 3/4).
ARMOR_STATS = {
    "cloth": [("int", 0.3), ("wis", 0.25), ("mana", 0.3), ("sta", 0.15)],
    "leather": [("agi", 0.3), ("dex", 0.2), ("sta", 0.2), ("hp", 0.2), ("wis", 0.1)],
    "chain": [("str", 0.25), ("dex", 0.15), ("sta", 0.25), ("hp", 0.25), ("wis", 0.1)],
    "plate": [("sta", 0.35), ("str", 0.3), ("hp", 0.35)],
}
RESIST_CYCLE = ["fire", "cold", "magic", "poison", "disease"]


def tier_rarity(i: int, n: int) -> str:
    third = n / 3.0
    return "common" if i < third else ("uncommon" if i < 2 * third else "rare")


def value_of(level: int, share: float, rarity: str) -> int:
    return int(round(5 + level * level * 3.0 * share * 5.0 * RARITY_VALUE[rarity]))


def stat_block(budget: float, split: list[tuple[str, float]]) -> tuple[dict, int, int]:
    stats: dict[str, int] = {}
    hp = mana = 0
    for key, share in split:
        pts = budget * share
        if key == "hp":
            hp += int(round(pts * 3))
        elif key == "mana":
            mana += int(round(pts * 4))
        else:
            v = int(round(pts))
            if v:
                stats[key] = stats.get(key, 0) + v
    return stats, hp, mana


def budget_for(level: int, rarity: str, share: float) -> float:
    return (2.0 + level * 0.35) * RARITY_STAT[rarity] * share * 5.0


def armor() -> list[dict]:
    rows = []
    for kind in ("cloth", "leather", "chain", "plate"):
        for t, lvl in enumerate(ARMOR_TIERS):
            rar = tier_rarity(t, len(ARMOR_TIERS))
            for si, slot in enumerate(ARMOR_SLOTS):
                share = SLOT_SHARE[slot]
                ac = max(1, int(round(ARMOR_WEIGHT[kind] * share * (1.5 * lvl + 5) * (1.0 + 0.1 * RARITY_STAT[rar]))))
                name = f"{MATERIAL[kind][t]} {NOUN[kind][slot]}"
                row = {"id": f"{kind}_t{t + 1}_{slot}", "name": name, "kind": "armor", "stack": 1, "value": value_of(lvl, share, rar),
                       "rarity": rar, "slots": [slot], "armor": kind, "ac": ac, "req_level": lvl}
                stats, hp, mana = stat_block(budget_for(lvl, rar, share), ARMOR_STATS[kind])
                if stats:
                    row["stats"] = stats
                if hp:
                    row["hp"] = hp
                if mana:
                    row["mana"] = mana
                if rar == "rare" and si % 3 == 0:
                    row["resists"] = {RESIST_CYCLE[(t + si) % 5]: 5 + 2 * t}
                if rar == "rare" and slot in ("hands", "wrist") and kind in ("leather", "chain", "plate"):
                    row["haste"] = 4 + 2 * (t - 5)
                if rar != "common":
                    row["magic"] = True
                row["description"] = f"{kind.capitalize()} armor for the {slot} (level {lvl})."
                rows.append(row)
    return rows


# ------------------------------------------------------------------------------------------- weapons

WEAPON_TIERS = [1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55]
METAL = ["Rusty", "Bronze", "Iron", "Steel", "Fine Steel", "Silversteel", "Runed", "Starmetal", "Duskforged", "Sunforged", "Ashen", "Emberforged"]
WOOD = ["Ashwood", "Yew", "Elmwood", "Ironwood", "Silverbark", "Heartwood", "Runewood", "Starwood", "Duskwood", "Sunwood", "Ashen", "Embergrain"]
CLOTHWRAP = ["Linen", "Leather", "Studded", "Ironbound", "Spiked", "Silverbound", "Runed", "Starbound", "Duskbound", "Sunbound", "Ashen", "Emberbound"]
# (key, noun, weapon type, delay, damage mult, material list, visual, stat split)
WEAPONS = [
    ("shortsword", "Shortsword", "1h_slash", 2.6, 1.0, METAL, "sword_basic", [("str", 0.4), ("dex", 0.3), ("sta", 0.3)]),
    ("longsword", "Longsword", "1h_slash", 3.0, 1.0, METAL, "sword_basic", [("str", 0.5), ("sta", 0.5)]),
    ("handaxe", "Hand Axe", "1h_slash", 3.2, 1.02, METAL, "sword_basic", [("str", 0.6), ("sta", 0.4)]),
    ("greatsword", "Greatsword", "2h_slash", 4.0, 1.45, METAL, "sword_basic", [("str", 0.5), ("sta", 0.3), ("hp", 0.2)]),
    ("greataxe", "Greataxe", "2h_slash", 4.4, 1.48, METAL, "sword_basic", [("str", 0.6), ("sta", 0.2), ("hp", 0.2)]),
    ("dagger", "Dagger", "1h_pierce", 2.2, 0.95, METAL, "dagger_basic", [("dex", 0.5), ("agi", 0.5)]),
    ("rapier", "Rapier", "1h_pierce", 2.6, 0.97, METAL, "dagger_basic", [("dex", 0.4), ("agi", 0.3), ("cha", 0.3)]),
    ("spear", "Spear", "2h_pierce", 3.8, 1.42, METAL, "staff_basic", [("str", 0.4), ("dex", 0.3), ("sta", 0.3)]),
    ("mace", "Mace", "1h_blunt", 3.0, 1.0, METAL, "mace_basic", [("str", 0.4), ("wis", 0.3), ("sta", 0.3)]),
    ("warhammer", "Warhammer", "1h_blunt", 3.4, 1.03, METAL, "mace_basic", [("str", 0.6), ("sta", 0.4)]),
    ("staff", "Staff", "2h_blunt", 3.6, 1.3, WOOD, "staff_basic", [("int", 0.35), ("wis", 0.35), ("mana", 0.3)]),
    ("maul", "Maul", "2h_blunt", 4.4, 1.48, METAL, "staff_basic", [("str", 0.6), ("sta", 0.4)]),
    ("handwraps", "Handwraps", "hand_to_hand", 2.4, 0.95, CLOTHWRAP, "", [("agi", 0.4), ("str", 0.3), ("sta", 0.3)]),
    ("shortbow", "Shortbow", "archery", 3.0, 0.9, WOOD, "bow_on_back", [("dex", 0.6), ("agi", 0.4)]),
    ("longbow", "Longbow", "archery", 3.8, 0.95, WOOD, "bow_on_back", [("dex", 0.5), ("str", 0.5)]),
    ("throwing_axe", "Throwing Axes", "throwing", 2.8, 0.8, METAL, "", [("dex", 0.5), ("str", 0.5)]),
]
PROC_SCHOOLS = [("fire", "Searing Burst", "fire"), ("cold", "Rime Strike", "cold"), ("magic", "Arcane Jolt", "magic"), ("poison", "Venom Sting", "poison")]


def weapon_damage(level: int, delay: float, mult: float, rarity: str) -> int:
    dps = (4.0 + 1.15 * (level + 2.5)) / 3.0
    return max(1, int(round(dps * delay * mult * RARITY_DAMAGE[rarity])))


def proc_effect_id(school: str, level: int) -> str:
    return f"proc_{school}_{level}"


def weapons(effects: dict) -> list[dict]:
    rows = []
    for key, noun, wtype, delay, mult, mats, visual, split in WEAPONS:
        for t, lvl in enumerate(WEAPON_TIERS):
            rar = tier_rarity(t, len(WEAPON_TIERS))
            ranged = wtype in ("archery", "throwing")
            slots = ["ranged"] if ranged else (["primary"] if wtype.startswith("2h") else ["primary", "secondary"])
            share = 0.25 if wtype.startswith("2h") else 0.18
            row = {"id": f"{key}_t{t + 1}", "name": f"{mats[t]} {noun}", "kind": "weapon", "stack": 1, "value": value_of(lvl, share, rar), "rarity": rar,
                   "slots": slots, "weapon": wtype, "damage": weapon_damage(lvl, delay, mult, rar), "delay": delay, "req_level": lvl}
            if ranged:
                row["range"] = 40.0 if key == "longbow" else (30.0 if key == "shortbow" else 20.0)
            if visual:
                row["visual"] = visual
            stats, hp, mana = stat_block(budget_for(lvl, rar, share), split)
            if stats:
                row["stats"] = stats
            if hp:
                row["hp"] = hp
            if mana:
                row["mana"] = mana
            if rar != "common":
                row["magic"] = True
            # Rare weapons (and every other uncommon one) carry a proc of their tier.
            if not ranged and (rar == "rare" or (rar == "uncommon" and t % 2 == 1)):
                school, pname, resist = PROC_SCHOOLS[(t + len(key)) % len(PROC_SCHOOLS)]
                eid = proc_effect_id(school, lvl)
                effects[eid] = proc_effect(eid, pname, resist, lvl)
                row["proc"] = {"effect": eid, "rate": 0.05}
            kind_text = {"1h_slash": "one-handed blade", "2h_slash": "two-handed blade", "1h_pierce": "piercing weapon", "2h_pierce": "polearm",
                         "1h_blunt": "one-handed bludgeon", "2h_blunt": "two-handed bludgeon", "hand_to_hand": "hand-to-hand weapon",
                         "archery": "bow", "throwing": "set of throwing weapons"}[wtype]
            extra = " It sometimes unleashes " + effects[row["proc"]["effect"]]["name"] + "." if "proc" in row else ""
            row["description"] = f"A {kind_text} (level {lvl}).{extra}"
            rows.append(row)
    return rows


def proc_effect(eid: str, name: str, resist: str, level: int) -> dict:
    return {"id": eid, "name": name, "class": "item", "level": 1, "type": "nuke", "resource": "none", "cost": 0, "cast_time": 0, "cooldown": 0,
            "gcd": False, "range": 10, "target": "enemy", "resist": resist,
            "effects": [{"type": "damage", "base": int(round(6 + 2.6 * level)), "per_level": 0}],
            "description": f"A burst of {resist} that strikes for about {int(round(6 + 2.6 * level))}."}


SHIELDS = [("buckler", "Buckler", 0.8), ("round_shield", "Round Shield", 1.0), ("kite_shield", "Kite Shield", 1.25)]


def shields() -> list[dict]:
    rows = []
    for key, noun, acm in SHIELDS:
        for t, lvl in enumerate(ARMOR_TIERS):
            rar = tier_rarity(t, len(ARMOR_TIERS))
            ac = max(1, int(round(acm * SLOT_SHARE["shield"] * (1.5 * lvl + 5) * (1.0 + 0.1 * RARITY_STAT[rar]))))
            row = {"id": f"{key}_t{t + 1}", "name": f"{METAL[min(11, t + t // 2)]} {noun}", "kind": "shield", "stack": 1,
                   "value": value_of(lvl, SLOT_SHARE["shield"], rar), "rarity": rar, "slots": ["secondary"], "weapon": "shield", "ac": ac,
                   "req_level": lvl, "visual": "shield_round"}
            stats, hp, _ = stat_block(budget_for(lvl, rar, 0.12), [("sta", 0.6), ("hp", 0.4)])
            if stats:
                row["stats"] = stats
            if hp:
                row["hp"] = hp
            if rar != "common":
                row["magic"] = True
            row["description"] = f"A shield for the off hand (level {lvl})."
            rows.append(row)
    return rows


# ------------------------------------------------------------------------------------------- jewelry

GEM = ["Copper", "Silver", "Electrum", "Gold", "Jade", "Moonstone", "Starstone", "Emberstone"]
JEWELRY = [("neck", "Pendant", "Torc"), ("ear", "Earring", "Stud"), ("finger", "Ring", "Band"), ("face", "Veil", "Mask"), ("back", "Shawl", "Cloak")]
FLAVOR = {"mystic": [("int", 0.3), ("wis", 0.3), ("mana", 0.4)], "martial": [("str", 0.3), ("sta", 0.35), ("hp", 0.35)]}


def jewelry() -> list[dict]:
    rows = []
    for slot, mystic_noun, martial_noun in JEWELRY:
        for flavor, noun in (("mystic", mystic_noun), ("martial", martial_noun)):
            for t, lvl in enumerate(ARMOR_TIERS):
                rar = tier_rarity(t, len(ARMOR_TIERS))
                share = SLOT_SHARE[slot]
                prefix = GEM[t] if slot != "back" else MATERIAL["cloth"][t]
                row = {"id": f"{flavor}_{slot}_t{t + 1}", "name": f"{prefix} {noun}", "kind": "jewelry", "stack": 1,
                       "value": value_of(lvl, share, rar) * 2, "rarity": rar, "slots": [slot], "req_level": lvl,
                       "ac": max(0, int(round(share * (0.5 * lvl + 2)))) if slot in ("back", "face") else 0}
                stats, hp, mana = stat_block(budget_for(lvl, rar, share) + (1.5 if rar == "common" else 0.0), FLAVOR[flavor])
                if stats:
                    row["stats"] = stats
                if hp:
                    row["hp"] = hp
                if mana:
                    row["mana"] = mana
                if t >= 3:
                    row["resists"] = {RESIST_CYCLE[(t + len(slot)) % 5]: 3 + 2 * t}
                if rar != "common":
                    row["magic"] = True
                if row["ac"] == 0:
                    del row["ac"]
                row["description"] = f"{'Mystic' if flavor == 'mystic' else 'Martial'} jewelry for the {slot} (level {lvl})."
                rows.append(row)
    return rows


# ------------------------------------------------------------------------------------------- consumables, bags, ammo

def fx(eid, name, typ, target, effects, desc, cooldown=0, gcd=False, resist="none", rng=0) -> dict:
    return {"id": eid, "name": name, "class": "item", "level": 1, "type": typ, "resource": "none", "cost": 0, "cast_time": 0, "cooldown": cooldown,
            "gcd": gcd, "range": rng, "target": target, "resist": resist, "stack_group": None, "effects": effects, "description": desc}


FOOD = [("trail_rations", "Trail Rations", 1, 5), ("roast_haunch", "Roast Haunch", 10, 40), ("hearty_stew", "Hearty Stew", 20, 120),
        ("spiced_meat_pie", "Spiced Meat Pie", 35, 400), ("feast_loaf", "Feast Loaf", 50, 900)]
DRINK = [("water_flask", "Water Flask", 1, 5), ("spring_water", "Spring Water", 10, 40), ("honeyed_tea", "Honeyed Tea", 20, 120),
         ("clarity_draught", "Clarity Draught", 35, 400), ("moonwell_water", "Moonwell Water", 50, 900)]
HEALING = [("minor_draught", "Minor Healing Draught", 1, 40, 25), ("lesser_draught", "Lesser Healing Draught", 10, 120, 90),
           ("healing_draught", "Healing Draught", 20, 280, 260), ("greater_draught", "Greater Healing Draught", 30, 500, 700),
           ("superior_draught", "Superior Healing Draught", 40, 800, 1500), ("supreme_draught", "Supreme Healing Draught", 50, 1200, 3000)]
MANA = [("minor_tonic", "Minor Mana Tonic", 1, 10, 30), ("lesser_tonic", "Lesser Mana Tonic", 12, 13, 110), ("mana_tonic", "Mana Tonic", 25, 16, 350),
        ("greater_tonic", "Greater Mana Tonic", 38, 20, 900), ("superior_tonic", "Superior Mana Tonic", 50, 25, 2000)]
BANDAGES = [("linen_bandage", "Linen Bandage", 1, 30, 8), ("wool_bandage", "Wool Bandage", 15, 110, 40), ("silk_bandage", "Silk Bandage", 30, 260, 150),
            ("runeweave_bandage", "Runeweave Bandage", 45, 480, 400)]
ELIXIRS = [("elixir_of_might", "Elixir of Might", [("str", 12)]), ("elixir_of_fortitude", "Elixir of Fortitude", [("sta", 12)]),
           ("elixir_of_insight", "Elixir of Insight", [("int", 10), ("wis", 10)]), ("elixir_of_grace", "Elixir of Grace", [("agi", 10), ("dex", 10)])]


def consumables(effects: dict) -> list[dict]:
    rows = []
    for i, (iid, name, lvl, value) in enumerate(FOOD):
        eid = f"fx_{iid}"
        regen = round(0.5 + lvl * 0.12, 1)
        effects[eid] = fx(eid, "Well Fed", "buff", "self", [{"type": "buff", "stat": "hp_regen", "value": regen, "duration": 1800}],
                          f"Health regeneration +{regen:g} per second for 30 minutes.")
        effects[eid]["stack_group"] = "well_fed"
        effects[eid]["rank"] = i + 1
        rows.append({"id": iid, "name": name, "kind": "consumable", "stack": 20, "value": value, "rarity": "common", "req_level": lvl,
                     "click": {"effect": eid, "cast_time": 0, "recast": 0},
                     "description": f"Food. Eating it adds {regen:g} health regeneration per second for 30 minutes."})
    for i, (iid, name, lvl, value) in enumerate(DRINK):
        eid = f"fx_{iid}"
        regen = round(0.3 + lvl * 0.08, 1)
        effects[eid] = fx(eid, "Refreshed", "buff", "self", [{"type": "buff", "stat": "mana_regen", "value": regen, "duration": 1800}],
                          f"Mana regeneration +{regen:g} per second for 30 minutes.")
        effects[eid]["stack_group"] = "refreshed"
        effects[eid]["rank"] = i + 1
        rows.append({"id": iid, "name": name, "kind": "consumable", "stack": 20, "value": value, "rarity": "common", "req_level": lvl,
                     "click": {"effect": eid, "cast_time": 0, "recast": 0},
                     "description": f"Drink. It adds {regen:g} mana regeneration per second for 30 minutes."})
    for iid, name, lvl, heal, value in HEALING:
        eid = f"fx_{iid}"
        effects[eid] = fx(eid, name, "heal", "self", [{"type": "heal", "base": heal, "per_level": 0}], f"Restores {heal} health.", cooldown=30)
        rows.append({"id": iid, "name": name, "kind": "consumable", "stack": 10, "value": value, "rarity": "uncommon" if lvl >= 20 else "common",
                     "req_level": lvl, "click": {"effect": eid, "cast_time": 0, "recast": 30},
                     "description": f"A red potion that restores {heal} health. 30 seconds between draughts."})
    for iid, name, lvl, pct, value in MANA:
        eid = f"fx_{iid}"
        effects[eid] = fx(eid, name, "utility", "self", [{"type": "restore_mana_pct", "value": pct}], f"Restores {pct}% of your mana.", cooldown=60)
        rows.append({"id": iid, "name": name, "kind": "consumable", "stack": 10, "value": value, "rarity": "uncommon" if lvl >= 25 else "common",
                     "req_level": lvl, "click": {"effect": eid, "cast_time": 0, "recast": 60},
                     "description": f"A blue tonic that restores {pct}% of your mana. A minute between draughts."})
    for iid, name, lvl, heal, value in BANDAGES:
        eid = f"fx_{iid}"
        effects[eid] = fx(eid, name, "heal", "self", [{"type": "heal", "base": heal, "per_level": 0}], f"Binds wounds for {heal} health.", cooldown=10)
        rows.append({"id": iid, "name": name, "kind": "consumable", "stack": 20, "value": value, "rarity": "common", "req_level": lvl,
                     "click": {"effect": eid, "cast_time": 4.0, "recast": 10},
                     "description": f"Takes 4 seconds to apply (moving or being struck can spoil it) and restores {heal} health."})
    for iid, name, stats in ELIXIRS:
        eid = f"fx_{iid}"
        effects[eid] = fx(eid, name, "buff", "self", [{"type": "buff", "stat": s, "value": v, "duration": 1800} for s, v in stats],
                          ", ".join(f"+{v} {s.upper()}" for s, v in stats) + " for 30 minutes.")
        rows.append({"id": iid, "name": name, "kind": "consumable", "stack": 10, "value": 600, "rarity": "uncommon", "req_level": 20,
                     "click": {"effect": eid, "cast_time": 0, "recast": 0},
                     "description": ", ".join(f"+{v} {s.upper()}" for s, v in stats) + " for 30 minutes."})
    # The Magician's conjured food and the flame of the conjured blade (items in gen_classes.py).
    effects["fx_conjured_bread"] = fx("fx_conjured_bread", "Well Fed", "buff", "self", [{"type": "buff", "stat": "hp_regen", "value": 2.0, "duration": 1800}],
                                      "Health regeneration +2 per second for 30 minutes.")
    effects["fx_conjured_bread"]["stack_group"] = "well_fed"
    effects["fx_conjured_bread"]["rank"] = 2
    effects["fx_conjured_water"] = fx("fx_conjured_water", "Refreshed", "buff", "self",
                                      [{"type": "buff", "stat": "mana_regen", "value": 1.5, "duration": 1800}], "Mana regeneration +1.5 per second for 30 minutes.")
    effects["fx_conjured_water"]["stack_group"] = "refreshed"
    effects["fx_conjured_water"]["rank"] = 2
    effects["fx_conjured_flame"] = proc_effect("fx_conjured_flame", "Conjured Flame", "fire", 18)
    return rows


BAGS = [("cloth_pouch", "Cloth Pouch", 4, 20, "common"), ("small_sack", "Small Sack", 6, 150, "common"), ("leather_satchel", "Leather Satchel", 8, 900, "common"),
        ("travelers_pack", "Traveler's Pack", 10, 4000, "common"), ("sturdy_backpack", "Sturdy Backpack", 12, 15000, "uncommon"),
        ("reinforced_haversack", "Reinforced Haversack", 14, 45000, "uncommon"), ("bottomless_satchel", "Bottomless Satchel", 16, 150000, "rare")]


def bags() -> list[dict]:
    return [{"id": iid, "name": name, "kind": "bag", "stack": 1, "value": value, "rarity": rar, "bag_slots": slots,
             "description": f"A bag with room for {slots} items. Bags cannot hold other bags."} for iid, name, slots, value, rar in BAGS]


ARROWS = [("rough_arrows", "Rough Arrows", 1, 1, 1), ("fletched_arrows", "Fletched Arrows", 10, 2, 3), ("barbed_arrows", "Barbed Arrows", 20, 4, 8),
          ("steelhead_arrows", "Steelhead Arrows", 30, 6, 20), ("runed_arrows", "Runed Arrows", 40, 8, 45), ("starshot_arrows", "Starshot Arrows", 50, 11, 90)]


def ammo() -> list[dict]:
    return [{"id": iid, "name": name, "kind": "ammo", "stack": 100, "value": value, "rarity": "common", "slots": ["ammo"], "weapon": "archery",
             "damage": dmg, "req_level": lvl, "description": f"Arrows for the ammo slot: each archery shot hits a little harder (+{dmg} damage, shared)."}
            for iid, name, lvl, dmg, value in ARROWS]


TRAINING = [
    {"id": "training_sword", "name": "Training Sword", "kind": "weapon", "stack": 1, "value": 4, "rarity": "common", "visual": "sword_basic",
     "slots": ["primary", "secondary"], "weapon": "1h_slash", "damage": 6, "delay": 3.0, "description": "A blunted practice blade. Better than bare hands."},
    {"id": "training_shield", "name": "Training Shield", "kind": "shield", "stack": 1, "value": 4, "rarity": "common", "visual": "shield_round",
     "slots": ["secondary"], "weapon": "shield", "ac": 2, "description": "A dented round shield."},
    {"id": "training_mace", "name": "Training Mace", "kind": "weapon", "stack": 1, "value": 4, "rarity": "common", "visual": "mace_basic",
     "slots": ["primary", "secondary"], "weapon": "1h_blunt", "damage": 6, "delay": 3.0, "description": "A wooden-hafted practice mace."},
    {"id": "training_staff", "name": "Apprentice Staff", "kind": "weapon", "stack": 1, "value": 4, "rarity": "common", "visual": "staff_basic",
     "slots": ["primary"], "weapon": "2h_blunt", "damage": 8, "delay": 3.6, "description": "A plain staff tipped with a dull focus stone."},
    {"id": "training_dagger", "name": "Training Dagger", "kind": "weapon", "stack": 1, "value": 4, "rarity": "common", "visual": "dagger_basic",
     "slots": ["primary", "secondary"], "weapon": "1h_pierce", "damage": 4, "delay": 2.2, "description": "A short practice knife."},
    {"id": "training_offhand_dagger", "name": "Off-hand Training Dagger", "kind": "weapon", "stack": 1, "value": 4, "rarity": "common",
     "visual": "dagger_offhand", "slots": ["primary", "secondary"], "weapon": "1h_pierce", "damage": 4, "delay": 2.2,
     "description": "A second practice knife, balanced for the off hand."},
    {"id": "training_bow", "name": "Training Bow", "kind": "weapon", "stack": 1, "value": 4, "rarity": "common", "visual": "bow_on_back",
     "slots": ["ranged"], "weapon": "archery", "damage": 6, "delay": 3.0, "range": 30.0, "description": "A short hunting bow for the ranged slot."},
]

# ------------------------------------------------------------------------------------------- named dungeon items

# The dungeons' named items (Phase 6 binds them to bosses and named rares): (dungeon id, display, boss level, raid tier)
DUNGEONS = [("rattlebone_warren", "Rattlebone Warren", 11, False), ("mirefang_hollow", "Mirefang Hollow", 17, False),
            ("cinderdeep_mines", "Cinderdeep Mines", 27, False), ("hollowmere_crypts", "Hollowmere Crypts", 33, False),
            ("stormspire_ruins", "Stormspire Ruins", 41, False), ("gloomroot_caverns", "Gloomroot Caverns", 46, False),
            ("obsidian_citadel", "Obsidian Citadel", 58, True), ("emberwyrm_lair", "Emberwyrm Lair", 60, True),
            # Phase 6 added two more dungeons (one per crowded band).
            ("sunken_bastion", "the Sunken Bastion", 24, False), ("deathless_sanctum", "the Deathless Sanctum", 53, False)]
NAMED = [
    # (suffix, name pattern, kind spec)
    ("blade", "{} Fang", ("weapon", "1h_slash", 2.8, 1.0, "sword_basic", ["primary", "secondary"])),
    ("staff", "Staff of {}", ("weapon", "2h_blunt", 3.6, 1.3, "staff_basic", ["primary"])),
    ("bow", "{} Recurve", ("weapon", "archery", 3.4, 0.95, "bow_on_back", ["ranged"])),
    ("plate", "{} Warplate", ("armor", "plate", "chest")),
    ("robe", "{} Vestments", ("armor", "cloth", "chest")),
    ("ring", "Signet of {}", ("jewelry", "finger")),
]
NAMED_WORDS = {"rattlebone_warren": "the Rattle King", "mirefang_hollow": "Mirefang", "cinderdeep_mines": "the Cinder Foreman",
               "hollowmere_crypts": "the Pale Abbot", "stormspire_ruins": "the Storm Herald", "gloomroot_caverns": "the Root Mother",
               "obsidian_citadel": "the Obsidian Tyrant", "emberwyrm_lair": "the Emberwyrm", "sunken_bastion": "the Drowned Admiral",
               "deathless_sanctum": "the High Priest"}
NAMED_SHORT = {"rattlebone_warren": "Rattlebone", "mirefang_hollow": "Mirefang", "cinderdeep_mines": "Cinderdeep", "hollowmere_crypts": "Hollowmere",
               "stormspire_ruins": "Stormspire", "gloomroot_caverns": "Gloomroot", "obsidian_citadel": "Obsidian", "emberwyrm_lair": "Emberwyrm",
               "sunken_bastion": "Saltgrave", "deathless_sanctum": "Deathless"}


def named(effects: dict) -> list[dict]:
    rows = []
    for di, (did, display, lvl, raid) in enumerate(DUNGEONS):
        rar = "epic" if raid else "rare"
        word = NAMED_SHORT[did] if di % 2 == 0 else NAMED_WORDS[did].replace("the ", "").title()
        for k, (suffix, pattern, spec) in enumerate(NAMED):
            label = pattern.format(word if "{} " in pattern else NAMED_WORDS[did])
            iid = f"{did.split('_')[0]}_{suffix}"
            row = {"id": iid, "name": label[:48], "stack": 1, "rarity": rar, "req_level": max(1, lvl - 4), "magic": True, "lore": True,
                   "source": f"dungeon:{did}"}
            if raid:
                row["no_trade"] = True
            if spec[0] == "weapon":
                _, wtype, delay, mult, visual, slots = spec
                row.update({"kind": "weapon", "weapon": wtype, "delay": delay, "damage": weapon_damage(lvl, delay, mult, rar) + 1, "slots": slots,
                            "visual": visual, "value": value_of(lvl, 0.25, rar)})
                if wtype == "archery":
                    row["range"] = 45.0
                stats, hp, mana = stat_block(budget_for(lvl, rar, 0.3), [("str", 0.3), ("dex", 0.3), ("sta", 0.4)] if wtype != "2h_blunt"
                                             else [("int", 0.35), ("wis", 0.35), ("mana", 0.3)])
                if wtype != "archery":
                    school, pname, resist = PROC_SCHOOLS[di % len(PROC_SCHOOLS)]
                    eid = proc_effect_id(school, lvl + 3)
                    effects[eid] = proc_effect(eid, pname, resist, lvl + 3)
                    row["proc"] = {"effect": eid, "rate": 0.07 if raid else 0.06}
            elif spec[0] == "armor":
                _, kind, slot = spec
                share = SLOT_SHARE[slot]
                row.update({"kind": "armor", "armor": kind, "slots": [slot], "value": value_of(lvl, share, rar),
                            "ac": int(round(ARMOR_WEIGHT[kind] * share * (1.5 * lvl + 5) * 1.3))})
                stats, hp, mana = stat_block(budget_for(lvl, rar, share), ARMOR_STATS[kind])
                row["resists"] = {RESIST_CYCLE[di % 5]: 10 + di * 2}
            else:
                _, slot = spec
                share = SLOT_SHARE[slot]
                row.update({"kind": "jewelry", "slots": [slot], "value": value_of(lvl, share, rar) * 3})
                stats, hp, mana = stat_block(budget_for(lvl, rar, 0.15), [("str", 0.2), ("sta", 0.2), ("int", 0.2), ("wis", 0.2), ("hp", 0.2)])
                if di >= 4:
                    row["haste"] = 5 + di
            if stats:
                row["stats"] = stats
            if hp:
                row["hp"] = hp
            if mana:
                row["mana"] = mana
            row["description"] = f"Named loot of {display}: its boss and named rares carry it."
            rows.append(row)
    return rows


# ------------------------------------------------------------------------------------------- quest items and epic weapons

def quest_items_and_epics(effects: dict) -> list[dict]:
    """Quest items and each class's epic weapon (defined in tools/gen_quests.py, the quests that hand them out)."""
    from gen_quests import EPICS, QUEST_ITEMS
    rows = [{"id": iid, "name": name, "kind": "quest", "stack": 10, "value": 0, "no_trade": True, "description": desc}
            for iid, name, desc in QUEST_ITEMS]
    lvl = 46
    splits = {"melee": [("str", 0.35), ("dex", 0.25), ("sta", 0.25), ("hp", 0.15)],
              "hybrid": [("str", 0.3), ("sta", 0.25), ("wis", 0.2), ("hp", 0.15), ("mana", 0.1)],
              "caster": [("int", 0.3), ("wis", 0.3), ("sta", 0.15), ("mana", 0.25)]}
    for cid, (iid, name, wtype, delay, slots, visual, school, emphasis) in sorted(EPICS.items()):
        mult = 0.85 if emphasis == "caster" else 1.15
        row = {"id": iid, "name": name, "kind": "weapon", "weapon": wtype, "delay": delay, "damage": weapon_damage(lvl, delay, mult, "epic") + 2,
               "slots": slots, "stack": 1, "rarity": "epic", "req_level": lvl, "magic": True, "lore": True, "no_trade": True, "classes": [cid],
               "value": value_of(lvl, 0.3, "epic"), "source": f"quest:epic_{cid}"}  # a source keeps it out of loot pools
        if visual:
            row["visual"] = visual
        if wtype == "archery":
            row["range"] = 50.0
        stats, hp, mana = stat_block(budget_for(lvl, "epic", 0.45), splits[emphasis])
        if stats:
            row["stats"] = stats
        if hp:
            row["hp"] = hp
        if mana:
            row["mana"] = mana
        if school:
            resist = {"fire": "fire", "cold": "cold", "magic": "magic", "poison": "poison", "disease": "disease"}[school]
            eid = proc_effect_id(school, lvl + 8)
            pname = {"fire": "Searing Burst", "cold": "Rime Strike", "magic": "Arcane Jolt", "poison": "Venom Sting", "disease": "Withering Touch"}[school]
            effects[eid] = proc_effect(eid, pname, resist, lvl + 8)
            row["proc"] = {"effect": eid, "rate": 0.09}
        row["haste"] = 12 if emphasis != "caster" else 0
        if not row["haste"]:
            row.pop("haste")
        row["description"] = f"The epic weapon of the {cid.replace('_', ' ')}s, earned at the end of their order's trials."
        rows.append(row)
    return rows


# ------------------------------------------------------------------------------------------- vendors, loot

def vendors(items: list[dict]) -> list[dict]:
    by_id = {i["id"]: i for i in items}

    def line(iid: str, count: int = -1, restock: float = 0.0) -> dict:
        assert iid in by_id, iid
        r = {"item": iid}
        if count > 0:
            r["count"] = count
            r["restock"] = restock
        return r

    def gear(armor_tiers: list[int], weapon_tiers: list[int], shield_tiers: list[int], limited: bool) -> list[dict]:
        out = []
        for kind in ("cloth", "leather", "chain", "plate"):
            for t in armor_tiers:
                for slot in ARMOR_SLOTS:
                    iid = f"{kind}_t{t}_{slot}"
                    out.append(line(iid, 2, 600.0) if limited or by_id[iid]["rarity"] != "common" else line(iid))
        for key, *_ in WEAPONS:
            for t in weapon_tiers:
                iid = f"{key}_t{t}"
                out.append(line(iid, 2, 600.0) if limited or by_id[iid]["rarity"] != "common" else line(iid))
        for key, *_ in SHIELDS:
            for t in shield_tiers:
                iid = f"{key}_t{t}"
                out.append(line(iid, 2, 600.0) if limited or by_id[iid]["rarity"] != "common" else line(iid))
        return out

    npc = []

    def place(nid, name, role, angle_deg, radius, greeting, stock=None, band=(0, 0), model="humanoid"):
        a = math.radians(angle_deg)
        x, z = round(radius * math.cos(a), 1), round(radius * math.sin(a), 1)
        row = {"id": nid, "name": name, "role": role, "model": model, "level": 40, "zone": ZONE, "x": x, "z": z,
               "yaw": round(math.atan2(-x, -z), 2), "classes": [], "sells": [], "greeting": greeting}
        if stock is not None:
            row["stock"] = stock
            row["band_min"], row["band_max"] = band
        npc.append(row)

    provisions = [line(i) for i in ("trail_rations", "roast_haunch", "hearty_stew", "water_flask", "spring_water", "honeyed_tea", "linen_bandage",
                                    "wool_bandage", "cloth_pouch", "small_sack", "leather_satchel", "rough_arrows", "fletched_arrows")]
    provisions += [line("spiced_meat_pie", 20, 120.0), line("clarity_draught", 20, 120.0), line("silk_bandage", 20, 120.0), line("travelers_pack", 3, 1800.0),
                   line("barbed_arrows", 200, 60.0), line("steelhead_arrows", 100, 120.0)]
    place("provisioner_brannigan", "Brannigan Holt", "vendor", 100, 24, "Food, drink, bandages, bags and arrows. Everything an adventurer forgets.",
          provisions, (1, 60))
    alchemy = [line("minor_draught"), line("lesser_draught"), line("minor_tonic"), line("lesser_tonic"), line("healing_draught", 10, 300.0),
               line("mana_tonic", 10, 300.0), line("greater_draught", 5, 600.0), line("greater_tonic", 5, 600.0)]
    alchemy += [line(i, 5, 900.0) for i, *_ in ELIXIRS]
    place("alchemist_oda", "Oda Fennick", "vendor", 125, 24, "Draughts for the wounded, tonics for the weary. Mind the fumes.", alchemy, (1, 60))
    place("outfitter_tamsin", "Tamsin Ironvale", "vendor", 150, 24, "New to the road? Start with honest iron and honest leather.",
          gear([1, 2], [1, 2, 3], [1, 2], False), (1, 15))
    place("smith_garrick", "Garrick Steelbough", "vendor", 175, 24, "Journeyman's work, sturdy and fair-priced.", gear([3, 4], [4, 5, 6], [3, 4], False),
          (15, 30))
    place("armorer_varra", "Varra Duskmantle", "vendor", 200, 24, "Veteran gear. I keep little stock; what I have is good.", gear([5, 6], [7, 8, 9], [5, 6], True),
          (30, 45))
    place("armsdealer_hesk", "Old Hesk", "vendor", 225, 24, "Master work. Pay the price or walk on.", gear([7], [10, 11], [7], True), (45, 60))
    jewels = []
    for slot, *_ in JEWELRY:
        for flavor in ("mystic", "martial"):
            for t in (1, 2, 3, 4):
                iid = f"{flavor}_{slot}_t{t}"
                jewels.append(line(iid) if by_id[iid]["rarity"] == "common" else line(iid, 2, 900.0))
    place("jeweler_lysa", "Lysa Brightcoin", "vendor", 250, 24, "Rings, pendants and earrings, each with a little something in it.", jewels, (1, 30))
    supplies = [line(iid, count, restock) for iid, count, restock in SUPPLIES]
    place(SUPPLIER_TEMPLATE, "Marta Kettleby", "vendor", 275, 24, "Coal, thread, salts, flour, herbs and the tools of every trade. The stations are yours to use.",
          supplies, (1, 60))
    place("banker_odell", "Odell Crane", "banker", 60, 22, "Your goods are safe with the Crane Exchange.")
    place("auctioneer_mirabel", "Mirabel Vane", "auctioneer", 80, 22, "Buy, sell, or browse. The board takes a small fee on every listing.")
    return npc


def trade_items(mobs: list[dict], existing: set) -> list[dict]:
    """Trade loot a mob family drops (pelts, scales, bones): a vendor-trash item per id, valued by the lowest level that drops it."""
    lowest: dict[str, int] = {}
    for m in mobs:
        t = m.get("trade")
        if t and t not in existing:
            lowest[t] = min(lowest.get(t, 99), m["level"])
    return [{"id": t, "name": t.replace("_", " ").title(), "kind": "loot", "stack": 20, "value": max(1, round(1 + lvl * lvl * 0.15)),
             "description": "Trade goods. Merchants pay for these."} for t, lvl in sorted(lowest.items())]


def loot_tables(items: list[dict], mobs: list[dict], old: dict, scrolls: list[dict], spell_levels: dict) -> list[dict]:
    tables = {t["id"]: t for t in old.get("loot_tables", [])}
    level_of = {m["loot"]: m["level"] for m in mobs if m.get("loot")}
    mob_of = {m["loot"]: m for m in mobs if m.get("loot")}
    named_by_dungeon: dict[str, list[str]] = {}
    for i in items:
        if str(i.get("source", "")).startswith("dungeon:"):
            named_by_dungeon.setdefault(i["source"].split(":", 1)[1], []).append(i["id"])
    # Every mob's table: an old one keeps its coin and drops; a new one gets coin by level and its family's trade loot,
    # and a dungeon's boss and named rares carry its named items.
    for tid, m in mob_of.items():
        lvl = m["level"]
        if tid not in tables:
            coin_max = round(2 + lvl * lvl * 0.45 * (4 if m.get("boss") else 2 if m.get("named") else 1))
            drops = []
            if m.get("trade"):
                drops.append({"item": m["trade"], "chance": 0.5, "min": 1, "max": 2 if lvl < 30 else 1})
            tables[tid] = {"id": tid, "coin_min": coin_max // 3, "coin_max": coin_max, "drops": drops}
        if m.get("dungeon") and (m.get("boss") or m.get("named")):
            named = named_by_dungeon.get(m["dungeon"], [])
            chance = 0.35 if m.get("boss") else 0.08
            keep = [d for d in tables[tid].get("drops", []) if d["item"] not in named]
            tables[tid]["drops"] = keep + [{"item": n, "chance": chance, "min": 1, "max": 1} for n in named]
    gear_items = [i for i in items if i.get("slots") and not i.get("source") and not i.get("summoned") and not i["id"].startswith("training_")]
    consum = [i for i in items if i["kind"] == "consumable" and not i.get("summoned")]
    out = []
    for tid in sorted(tables, key=lambda k: (level_of.get(k, 0), k)):
        t = tables[tid]
        lvl = level_of.get(tid, 1)
        boss = t["id"] == "grimjaw_loot" or bool(mob_of.get(tid, {}).get("boss")) or bool(mob_of.get(tid, {}).get("named"))
        c = [i for i in consum if i.get("req_level", 1) <= lvl and i.get("req_level", 1) >= max(1, lvl - 15)]
        # Trade loot stays; a direct consumable drop moves into the consumables pool only when that pool has it.
        pooled = {i["id"] for i in c}
        drops = [d for d in t.get("drops", []) if d["item"] not in pooled]
        lo = max(1, lvl - 8)
        g = [i for i in gear_items if lo <= i.get("req_level", 1) <= lvl]
        entries = [{"item": i["id"], "weight": RARITY_LOOT_WEIGHT[i["rarity"]] * (3.0 if boss and i["rarity"] == "rare" else 1.0)} for i in g]
        pools = []
        if entries:
            pools.append({"rolls": 2 if boss else 1, "chance": 1.0 if boss else round(min(0.35, 0.18 + lvl * 0.003), 3), "entries": entries})
        if c:
            pools.append({"rolls": 1, "chance": 0.15, "entries": [{"item": i["id"], "weight": 1.0, "min": 1, "max": 2 if i["stack"] >= 10 else 1}
                                                                   for i in c]})
        sc = [s for s in scrolls if max(1, lvl - 6) <= spell_levels.get(s["spell"], 99) <= lvl]
        if sc:
            pools.append({"rolls": 1, "chance": 0.05, "entries": [{"item": s["id"], "weight": 1.0} for s in sc]})
        # Phase 13: tradeskill materials, rolled last (only when the table still has room).
        if tid in mob_of:
            pools += drops_for(mob_of[tid])
        out.append({"id": t["id"], "coin_min": t["coin_min"], "coin_max": t["coin_max"], "max_drops": 6 if mob_of.get(tid, {}).get("boss") else 4 if boss else 3,
                    "drops": drops, "pools": pools})
    return out


# ------------------------------------------------------------------------------------------- output

def render(comment: str, key: str, rows: list[dict]) -> str:
    lines = ["{", '  "_comment": ' + json.dumps(comment) + ",", f'  "{key}": [']
    for i, r in enumerate(rows):
        lines.append("    " + json.dumps(r) + ("," if i < len(rows) - 1 else ""))
    lines += ["  ]", "}", ""]
    text = "\n".join(lines)
    json.loads(text)
    return text


def main() -> None:
    read = lambda n: json.loads((CONTENT / n).read_text(encoding="utf-8"))
    old_items = read("items.json")["items"]
    keep = [i for i in old_items if i.get("kind") == "scroll" or i.get("summoned")]
    trade_loot = [i for i in old_items if i.get("kind") == "loot"]
    effects: dict[str, dict] = {}
    gen = TRAINING + armor() + weapons(effects) + shields() + jewelry() + ammo() + consumables(effects) + bags() + named(effects) + \
        quest_items_and_epics(effects) + material_items()
    gen_ids = {i["id"] for i in gen}
    trade_loot = [i for i in trade_loot if i["id"] not in gen_ids]
    trade_loot += trade_items(read("mobs.json")["mobs"], gen_ids | {i["id"] for i in trade_loot})
    for i in trade_loot:
        i.setdefault("kind", "loot")
    items = trade_loot + gen + keep
    ids = [i["id"] for i in items]
    dupes = {x for x in ids if ids.count(x) > 1}
    if dupes:
        sys.exit(f"duplicate item ids: {sorted(dupes)}")
    for i in items:
        if len(i["id"]) > 32 or len(i["name"]) > 48:
            sys.exit(f"id/name too long: {i['id']} / {i['name']}")
    fx_rows = []
    for k in sorted(effects):
        e = dict(effects[k])
        if e.get("stack_group") is None:
            e.pop("stack_group", None)
        fx_rows.append(e)

    spells = read("spells.json")["spells"]
    spell_levels = {s["id"]: s["level"] for s in spells}
    scrolls = [i for i in keep if i.get("kind") == "scroll"]
    loot = loot_tables(items, read("mobs.json")["mobs"], read("loot.json"), scrolls, spell_levels)
    old_npcs = read("npcs.json")["npcs"]
    templates = vendors(items)
    npcs = ordered([n for n in old_npcs if n.get("role") not in ("vendor", "banker", "auctioneer") or n.get("origin") == "world"] + templates)
    # Phase 13: bought materials cost at least what their products sell back for (no buy-craft-sell loop). The
    # templates' stock is what every vendor sells (the cities' and outposts' vendors are copies of them).
    sold = {s["item"] for n in templates for s in n.get("stock", [])}
    balance_values({i["id"]: i for i in items}, sold, read("rules.json")["economy"])

    items_text = render("Items (generated by tools/gen_items.py; spell scrolls and summoned items by tools/gen_classes.py). value is in copper. "
                        "Worn items name their slots; weapons carry damage/delay; click effects and procs name content/item_effects.json ids. "
                        "See docs/DECISIONS.md (Phase 5) for the formulas.", "items", items)
    fx_text = render("Spell effects that items carry (generated by tools/gen_items.py): weapon procs, potions, food and drink, bandages, elixirs. "
                     "Cast by items, never learned. class is always 'item'.", "effects", fx_rows)
    loot_text = render("Loot tables (generated by tools/gen_items.py). Coin is in copper. 'drops' roll independently; each 'pools' entry is rolled "
                       "'rolls' times: first 'chance', then one weighted pick. At most max_drops item stacks drop in all.", "loot_tables", loot)
    npcs_text = render_npcs(npcs)
    outputs = (("items.json", items_text), ("item_effects.json", fx_text), ("loot.json", loot_text), ("npcs.json", npcs_text))
    if "--check" in sys.argv[1:]:
        stale = [n for n, text in outputs if not (CONTENT / n).exists() or (CONTENT / n).read_text(encoding="utf-8") != text]
        if stale:
            sys.exit("gen_items: stale generated content (run python tools/gen_items.py): " + ", ".join(stale))
        print("gen_items: generated content is up to date")
        return
    for n, text in outputs:
        (CONTENT / n).write_text(text, encoding="utf-8", newline="\n")
    non_scroll = [i for i in items if i["kind"] != "scroll"]
    by_kind: dict[str, int] = {}
    for i in non_scroll:
        by_kind[i["kind"]] = by_kind.get(i["kind"], 0) + 1
    print(f"wrote {len(items)} items ({len(non_scroll)} besides scrolls: {by_kind}), {len(fx_rows)} item effects, {len(loot)} loot tables, "
          f"{len(npcs)} npcs")


if __name__ == "__main__":
    main()
