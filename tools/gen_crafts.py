#!/usr/bin/env python3
"""Generate Realms of Emberfall's tradeskills (Phase 13).

Writes (deterministically; commit the output):
  content/crafts.json   the six tradeskills, their stations, the combine rules and every recipe

and defines what the other generators place:
  MATERIALS      the materials and tools (tools/gen_items.py adds them to content/items.json)
  DROPS          which monsters drop which raw materials (tools/gen_items.py adds them to loot tables)
  SUPPLIES       what a tradeskill supplier sells (tools/gen_items.py stocks the supplier vendor)
  STATIONS/YARDS the stations and where each city keeps them (tools/gen_world.py places them)

Design (docs/DECISIONS.md, Phase 13):
  * Six tradeskills, one station each. A skill runs 0..rules.max_skill and rises through combines.
  * Materials come in four tiers, one per level band (1-15, 15-30, 30-45, 45-60). Raw materials drop
    from fitting monsters (hides and meat from animals, ore from constructs and giants, herbs from
    plants, beasts and insects, gems from humanoids); the basics are sold by each city's supplier.
  * Recipes make the game's own items: armour, weapons, shields, jewellery, food and drink, potions,
    bandages, bags and arrows. A recipe's trivial (the skill past which it teaches nothing) follows
    the product's level: 10 + 4.8 x level, capped at 290. Uncommon pieces also take Arcane Dust and
    rare ones an Ember Essence, both rare drops.
  * Phase 16: recipes trivial at 50 or less are known from the start; the rest are taught by a recipe
    book per tradeskill and band of 50 (volumes II-VI). Volume II is sold by the city suppliers, volume
    III is the reward of each tradeskill's quest chain (tools/gen_quests.py) and a rare drop, and IV-VI
    drop from monsters of matching level (more often from named ones and bosses).

Usage: python tools/gen_crafts.py [--check]
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
OUT = CONTENT / "crafts.json"

RULES = {
    "max_skill": 300,          # every tradeskill's ceiling (no level cap)
    "combine_seconds": 2.5,    # how long one combine takes (Phase 16: moving, fighting or casting stops it)
    "success_max": 0.95,       # at or above a recipe's trivial
    "success_min": 0.05,       # the floor, however far below
    "success_per_point": 0.012,  # lost per point of skill below the trivial
    "skill_up_base": 0.2,      # skill-up chance one point below the trivial
    "skill_up_per_point": 0.01,  # added per further point below
    "skill_up_max": 0.6,
    "fail_loss_chance": 0.75,  # a failed combine loses each unit of material with this chance (tools never)
}

# (id, name, station kind, station name, station model, description)
TRADESKILLS = [
    ("smithing", "Smithing", "forge", "Forge", "station_forge",
     "Smelting ore into bars, and bars into chain and plate armour, metal weapons and shields."),
    ("tailoring", "Tailoring", "loom", "Loom", "station_loom",
     "Weaving cloth, curing hides into leather, and sewing cloth and leather armour, cloaks, bags and bandages."),
    ("cooking", "Cooking", "oven", "Oven", "station_oven", "Turning meat, flour and spices into food that keeps a traveller on their feet."),
    ("brewing", "Brewing", "brewing_vat", "Brewing Vat", "station_brewing",
     "Steeping herbs into drinks, healing draughts, mana tonics and elixirs."),
    ("fletching", "Fletching", "fletching_bench", "Fletching Bench", "station_fletching", "Shaping wood into shafts, arrows, bows and staves."),
    ("jewelcraft", "Jewelcraft", "jewelers_bench", "Jeweler's Bench", "station_jeweler", "Cutting gems and setting them in rings, pendants, earrings and masks."),
]

# The crafting yard of each city: its centre (a ring of stations and the supplier around it). Found clear of
# the hub scene's walls, props, NPCs, guard posts and entries (tools/world_check.py checks the spots).
YARDS = {"emberhold": (-2.0, 6.0), "thornwood": (-2.0, 2.0), "cragmouth": (6.0, 4.0)}
YARD_RADIUS = 7.0
SUPPLIER_TEMPLATE = "supplier_marta"
SUPPLIER_NAMES = {"emberhold": "Tradeswoman Hela Brindle", "thornwood": "Gatherer Wren Ashby", "cragmouth": "Supplier Grumm Coalhand"}


def yard_spots(hub: str) -> list[tuple[float, float, float]]:
    """Seven spots on the yard's ring (x, z, yaw facing the centre): the six stations, then the supplier."""
    cx, cz = YARDS[hub]
    out = []
    for k in range(7):
        a = 2.0 * math.pi * k / 7.0
        x, z = cx + YARD_RADIUS * math.cos(a), cz + YARD_RADIUS * math.sin(a)
        out.append((round(x, 1), round(z, 1), round(math.atan2(cx - x, cz - z), 4)))
    return out


def stations(hub: str) -> list[dict]:
    """The station NPCs of a city's yard (role "station"; tools/gen_world.py adds them to npcs.json)."""
    rows = []
    for (tid, _, kind, name, model, _), (x, z, yaw) in zip(TRADESKILLS, yard_spots(hub)):
        rows.append({"id": f"{hub}_{kind}", "name": name, "role": "station", "model": model, "level": 1, "zone": hub, "x": x, "z": z,
                     "yaw": yaw, "station": kind, "greeting": ""})
    return rows


def supplier_spot(hub: str) -> tuple[float, float, float]:
    return yard_spots(hub)[6]


# ------------------------------------------------------------------------------------------- materials

TIER_LEVEL = [1, 15, 30, 45]  # the level band each material tier serves
TIER_VALUE = [3, 15, 45, 120]
# Smelting, weaving, curing, shaping and cutting by tier: just below the tier's first products, so a list
# sorted by trivial starts each tier with what feeds it.
INTER_TRIVIAL = [10, 75, 145, 215]


def tier_of(level: int) -> int:
    return 0 if level < 15 else 1 if level < 30 else 2 if level < 45 else 3


def trivial_for(level: int) -> int:
    return min(290, int(round(10 + 4.8 * level)))


# Per tier: (raw id, raw name, refined id, refined name). None raw = an existing item.
ORE = [("copper_ore", "Copper Ore", "copper_bar", "Copper Bar"), ("iron_ore", "Iron Ore", "iron_bar", "Iron Bar"),
       ("duskiron_ore", "Duskiron Ore", "duskiron_bar", "Duskiron Bar"), ("starmetal_ore", "Starmetal Ore", "starmetal_bar", "Starmetal Bar")]
FIBER = [("flax_bundle", "Flax Bundle", "linen_bolt", "Linen Bolt"), ("wool_fleece", "Wool Fleece", "wool_bolt", "Wool Bolt"),
         ("spider_silk", None, "silk_bolt", "Silk Bolt"), ("wraithsilk_strand", "Wraithsilk Strand", "ghostweave_bolt", "Ghostweave Bolt")]
HIDE = [("rough_hide", "Rough Hide", "light_leather", "Light Leather"), ("thick_hide", "Thick Hide", "sturdy_leather", "Sturdy Leather"),
        ("heavy_hide", "Heavy Hide", "heavy_leather", "Heavy Leather"), ("pristine_hide", "Pristine Hide", "hardened_leather", "Hardened Leather")]
WOOD = [("ashwood_branch", "Ashwood Branch", "ashwood_shafts", "Ashwood Shafts"), ("yew_branch", "Yew Branch", "yew_shafts", "Yew Shafts"),
        ("living_wood", None, "livingwood_shafts", "Livingwood Shafts"),
        ("heartwood_branch", "Heartwood Branch", "heartwood_shafts", "Heartwood Shafts")]
GEMS = [("rough_agate", "Rough Agate", "cut_agate", "Cut Agate"), ("rough_garnet", "Rough Garnet", "cut_garnet", "Cut Garnet"),
        ("rough_sapphire", "Rough Sapphire", "cut_sapphire", "Cut Sapphire"),
        ("rough_emberstone", "Rough Emberstone", "cut_emberstone", "Cut Emberstone")]
HERBS = [("silverleaf", "Silverleaf"), ("bloodroot", "Bloodroot"), ("moonpetal", "Moonpetal"), ("starbloom", "Starbloom")]

RAW_TEXT = {
    "ore": "Ore for the forge. Smelt it into bars.", "fiber": "Fibre for the loom. Weave it into cloth.",
    "hide": "An untanned hide. Cure it at a loom with tanning salts.", "wood": "Wood for the fletching bench.",
    "gem": "An uncut stone. A jeweler can cut it.", "herb": "A herb for the brewing vat.",
}
REFINED_TEXT = {
    "ore": "A bar of metal, ready for the anvil.", "fiber": "A bolt of cloth for tailoring.", "hide": "Cured leather for tailoring and grips.",
    "wood": "A bundle of straight shafts for arrows.", "gem": "A cut stone, ready to set.",
}

# Supplies, tools and the rest: (id, name, value, stack, description)
SUPPLY_ITEMS = [
    ("forge_coal", "Forge Coal", 2, 20, "Coal that burns hot enough to smelt ore."),
    ("smithing_hammer", "Smithing Hammer", 60, 1, "A smith's hammer. Needed at the forge; never used up."),
    ("tanning_salts", "Tanning Salts", 2, 20, "Salts that cure a hide into leather."),
    ("spool_of_thread", "Spool of Thread", 2, 20, "Strong thread for sewing."),
    ("sewing_needle", "Sewing Needle", 40, 1, "A tailor's needle. Needed to sew garments; never used up."),
    ("bowstring", "Bowstring", 8, 20, "A waxed string, twisted from thread at a loom."),
    ("raw_meat", "Raw Meat", 3, 20, "Fresh meat from a beast. Cook it before it turns."),
    ("prime_cut", "Prime Cut", 30, 20, "Choice meat from a great beast."),
    ("flour_sack", "Flour Sack", 2, 20, "Milled flour for dough and crusts."),
    ("cooking_spices", "Cooking Spices", 4, 20, "A pouch of salt, pepper and dried herbs."),
    ("root_vegetables", "Root Vegetables", 3, 20, "Turnips, onions and carrots."),
    ("jar_of_honey", "Jar of Honey", 6, 20, "Golden honey from Thornwood's hives."),
    ("bread_dough", "Bread Dough", 6, 20, "Dough ready for the oven."),
    ("meat_stock", "Meat Stock", 10, 20, "A rich stock for stews."),
    ("honey_glaze", "Honey Glaze", 25, 20, "A sweet glaze of honey and spice."),
    ("pie_crust", "Pie Crust", 30, 20, "A flaky crust, waiting for a filling."),
    ("smoked_prime_cut", "Smoked Prime Cut", 80, 20, "Prime meat smoked over the oven's coals."),
    ("empty_vial", "Empty Vial", 2, 20, "A small glass vial."),
    ("goose_feathers", "Goose Feathers", 2, 20, "Grey feathers for fletching."),
    ("fletching_knife", "Fletching Knife", 40, 1, "A fletcher's knife. Needed to shape bows and staves; never used up."),
    ("jewelers_loupe", "Jeweler's Loupe", 60, 1, "A jeweler's lens. Needed to cut and set stones; never used up."),
    ("arcane_dust", "Arcane Dust", 60, 20, "Glittering dust left where magic has pooled. Uncommon crafted pieces need it."),
    ("ember_essence", "Ember Essence", 400, 20, "A warm, slowly pulsing mote. Rare crafted pieces need it."),
]
TOOLS = {"smithing": "smithing_hammer", "tailoring": "sewing_needle", "fletching": "fletching_knife", "jewelcraft": "jewelers_loupe"}

# Phase 16: recipe books. A recipe trivial at FREE_TRIVIAL or less is known from the start; above it, volume v
# teaches the recipes trivial in (50 (v - 1), 50 v]. (volume, value, rarity).
FREE_TRIVIAL = 50
BOOK_VOLUMES = [(2, 300, "common"), (3, 1500, "uncommon"), (4, 5000, "rare"), (5, 15000, "rare"), (6, 35000, "epic")]
ROMAN = {2: "II", 3: "III", 4: "IV", 5: "V", 6: "VI"}
BOOK_TITLES = {"smithing": "Forgemaster's Folio", "tailoring": "Weaver's Pattern Book", "cooking": "Hearthkeeper's Recipes",
               "brewing": "Brewer's Ledger", "fletching": "Fletcher's Notes", "jewelcraft": "Gemcutter's Codex"}
# Where volumes III-VI drop ("*": any monster): (volume, min level, max level, chance per book), plus a better
# chance on named monsters and bosses.
BOOK_DROPS = [(3, 15, 29, 0.004), (4, 25, 39, 0.003), (5, 35, 49, 0.003), (6, 45, 60, 0.002)]
BOOK_NAMED = [(3, 15, 29, 0.03), (4, 25, 39, 0.03), (5, 35, 49, 0.025), (6, 45, 60, 0.02)]


def book_id(ts: str, volume: int) -> str:
    return f"{ts}_book_{volume}"


def book_volume(trivial: int) -> int:
    """The volume that teaches a recipe of this trivial (1 = known from the start)."""
    return 1 if trivial <= FREE_TRIVIAL else min(6, (trivial - 1) // 50 + 1)


def book_items() -> list[dict]:
    rows = []
    for ts, *_ in TRADESKILLS:
        for vol, value, rarity in BOOK_VOLUMES:
            lo, hi = 50 * (vol - 1) + 1, 50 * vol
            rows.append({"id": book_id(ts, vol), "name": f"{BOOK_TITLES[ts]}, Volume {ROMAN[vol]}", "kind": "book", "stack": 1,
                         "value": value, "rarity": rarity, "tradeskill": ts,
                         "description": f"Read it to learn the {ts} recipes of trivial {lo} to {hi}. The book is spent; the knowledge stays."})
    return rows


# A master's piece per tradeskill, its one recipe trivial at max_skill, so every skill can be taught to its cap. They
# are made from top-tier drops and Ember Essence and do nothing but sell well (kind "loot"):
# (item, name, tradeskill, value, description).
MASTERWORKS = [
    ("emberwrought_showblade", "Emberwrought Showblade", "smithing", 6000,
     "A blade forged for display, never for battle. Only a master smith can fold starmetal this fine."),
    ("starloom_tapestry", "Starloom Tapestry", "tailoring", 6000,
     "A wall hanging of ghostweave and leather that shimmers like the night sky. A master weaver's piece."),
    ("hearthmasters_banquet", "Hearthmaster's Banquet", "cooking", 6000,
     "A feast fit for a lord's table, packed to travel. Only a master cook could prepare it."),
    ("vintage_ember_cordial", "Vintage Ember Cordial", "brewing", 6000,
     "A cordial that glows faintly in its bottle. Collectors pay well for a master brewer's vintage."),
    ("heartwood_totem", "Carved Heartwood Totem", "fletching", 6000,
     "A totem carved from heartwood and set with an emberstone. A master woodworker's showpiece."),
    ("starfire_mosaic", "Starfire Gem Mosaic", "jewelcraft", 6000,
     "Cut emberstones set in starmetal, arranged to catch any light. A master jeweller's piece."),
]


def material_items() -> list[dict]:
    """The new materials and tools for content/items.json (kind "tradeskill"), and the masterworks (kind "loot")."""
    rows = []

    def add(iid: str, name: str, value: int, desc: str, stack: int = 20) -> None:
        rows.append({"id": iid, "name": name, "kind": "tradeskill", "stack": stack, "value": value, "rarity": "common", "description": desc})

    for family, table in (("ore", ORE), ("fiber", FIBER), ("hide", HIDE), ("wood", WOOD), ("gem", GEMS)):
        for t, (raw, raw_name, ref, ref_name) in enumerate(table):
            if raw_name:
                add(raw, raw_name, TIER_VALUE[t], RAW_TEXT[family] + f" (tier {t + 1})")
            add(ref, ref_name, int(round(TIER_VALUE[t] * 2.5)), REFINED_TEXT[family] + f" (tier {t + 1})")
    for t, (hid, name) in enumerate(HERBS):
        add(hid, name, TIER_VALUE[t], RAW_TEXT["herb"] + f" (tier {t + 1})")
    for iid, name, value, stack, desc in SUPPLY_ITEMS:
        add(iid, name, value, desc, stack)
    for iid, name, _ts, value, desc in MASTERWORKS:
        rows.append({"id": iid, "name": name, "kind": "loot", "stack": 5, "value": value, "rarity": "rare", "description": desc})
    return rows + book_items()


# What the city suppliers sell: (item, count, restock seconds); count -1 = unlimited. The secondary supplies and the
# tools only: ore, meat, herbs and gems come from monsters, so most recipes need something hunted, and balance_values()
# prices the rest so that nothing made only from bought things sells back for a profit.
SUPPLIES = [
    ("forge_coal", -1, 0), ("smithing_hammer", -1, 0), ("tanning_salts", -1, 0), ("spool_of_thread", -1, 0),
    ("sewing_needle", -1, 0), ("flax_bundle", -1, 0), ("wool_fleece", 40, 90.0), ("flour_sack", -1, 0), ("cooking_spices", -1, 0),
    ("root_vegetables", -1, 0), ("jar_of_honey", -1, 0), ("water_flask", -1, 0), ("empty_vial", -1, 0),
    ("ashwood_branch", -1, 0), ("yew_branch", 30, 90.0), ("goose_feathers", -1, 0),
    ("fletching_knife", -1, 0), ("jewelers_loupe", -1, 0),
] + [(book_id(_ts[0], 2), -1, 0) for _ts in TRADESKILLS]  # Phase 16: every craft's second volume

# Monster drops, rolled after everything else in a mob's loot table: (body or "*", min level, max level, item,
# chance, min, max). Bosses and named rares also roll ESSENCE_NAMED.
DROPS = []
for _t, (_lo, _hi) in enumerate([(1, 14), (15, 29), (30, 44), (45, 60)]):
    DROPS.append(("animal", _lo, _hi, HIDE[_t][0], 0.35, 1, 1))
    DROPS.append(("construct", _lo, _hi, ORE[_t][0], 0.3, 1, 2))
    DROPS.append(("giant", _lo, _hi, ORE[_t][0], 0.25, 1, 2))
    DROPS.append(("elemental", _lo, _hi, ORE[_t][0], 0.3, 1, 2))
    DROPS.append(("humanoid", _lo, _hi, GEMS[_t][0], 0.06, 1, 1))
    for _body in ("plant", "insect", "animal"):
        DROPS.append((_body, _lo, _hi, HERBS[_t][0], 0.12, 1, 2))
DROPS += [
    # Low-tier ore for new smiths: beetles gnaw it from the rock, skeletons were miners once.
    ("insect", 1, 14, ORE[0][0], 0.2, 1, 2), ("undead", 1, 14, ORE[0][0], 0.15, 1, 1),
    ("insect", 15, 29, ORE[1][0], 0.2, 1, 2), ("undead", 15, 29, ORE[1][0], 0.15, 1, 1),
    ("animal", 1, 29, "raw_meat", 0.3, 1, 2), ("animal", 30, 60, "prime_cut", 0.3, 1, 1),
    ("undead", 40, 60, "wraithsilk_strand", 0.2, 1, 2),
    ("plant", 40, 60, "heartwood_branch", 0.3, 1, 2), ("giant", 40, 60, "heartwood_branch", 0.12, 1, 1),
    ("*", 20, 60, "arcane_dust", 0.04, 1, 1), ("*", 40, 60, "ember_essence", 0.025, 1, 1),
]
for _vol, _lo, _hi, _chance in BOOK_DROPS:  # Phase 16: recipe books, from anything of the right level
    for _ts in TRADESKILLS:
        DROPS.append(("*", _lo, _hi, book_id(_ts[0], _vol), _chance, 1, 1))
ESSENCE_NAMED = [(20, "arcane_dust", 0.5, 1, 3), (35, "ember_essence", 0.35, 1, 1)]


def drops_for(mob: dict) -> list[dict]:
    """Loot pools of materials for one mob (one single-entry pool per drop, rolled after the table's own)."""
    lvl, body = mob["level"], mob.get("body", "")
    out = []
    anyone: list[tuple[str, float, int, int]] = []
    for b, lo, hi, item, chance, n_min, n_max in DROPS:
        if not lo <= lvl <= hi:
            continue
        if b == body:
            out.append({"rolls": 1, "chance": chance, "entries": [{"item": item, "weight": 1.0, "min": n_min, "max": n_max}]})
        elif b == "*":
            anyone.append((item, chance, n_min, n_max))
    # The essences share a pool (one roll at their summed chance, then weighted by it): a table holds at most 8 pools.
    named = [(item, chance, n_min, n_max) for min_lvl, item, chance, n_min, n_max in ESSENCE_NAMED
             if lvl >= min_lvl and (mob.get("boss") or mob.get("named"))]
    # Phase 16: named monsters and bosses carry recipe books more often (their own pool, so the essences keep their odds).
    books = [(book_id(ts[0], vol), chance, 1, 1) for vol, lo, hi, chance in BOOK_NAMED if lo <= lvl <= hi for ts in TRADESKILLS
             if mob.get("boss") or mob.get("named")]
    for group in (anyone, named, books):
        if group:
            total = sum(c for _, c, _, _ in group)
            out.append({"rolls": 1, "chance": round(min(1.0, total), 3),
                        "entries": [{"item": i, "weight": round(c / total, 4), "min": lo, "max": hi} for i, c, lo, hi in group]})
    return out


# ------------------------------------------------------------------------------------------- recipes

ARMOR_TIERS = [1, 8, 15, 22, 30, 38, 46, 54]
WEAPON_TIERS = [1, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55]
ARMOR_SLOTS = ["head", "shoulders", "chest", "arms", "wrist", "hands", "waist", "legs", "feet"]
SLOT_COUNT = {"chest": 4, "legs": 3, "head": 2, "shoulders": 2, "feet": 2, "hands": 2, "arms": 2, "waist": 1, "wrist": 1}
METAL_WEAPONS = {"shortsword": 2, "longsword": 2, "handaxe": 2, "greatsword": 3, "greataxe": 3, "dagger": 1, "rapier": 2, "spear": 3, "mace": 2,
                 "warhammer": 2, "maul": 3, "throwing_axe": 2}
SHIELD_BARS = {"buckler": 2, "round_shield": 3, "kite_shield": 4}


def rarity_extra(rarity: str) -> list[tuple[str, int]]:
    return [("arcane_dust", 1)] if rarity == "uncommon" else [("ember_essence", 1)] if rarity == "rare" else []


class Book:
    def __init__(self, items: dict[str, dict]):
        self.items = items
        self.rows: list[dict] = []

    def add(self, skill: str, result: str, count: int, ingredients: list[tuple[str, int]], trivial: int, tools: tuple = ()) -> None:
        if result not in self.items:
            sys.exit(f"gen_crafts: a recipe makes unknown item '{result}'")
        for iid, _ in ingredients:
            if iid not in self.items:
                sys.exit(f"gen_crafts: the recipe for '{result}' uses unknown item '{iid}'")
        merged: dict[str, int] = {}
        for iid, n in ingredients:
            merged[iid] = merged.get(iid, 0) + n
        row = {"id": result, "tradeskill": skill, "trivial": trivial, "result": {"item": result, "count": count},
               "ingredients": [{"item": i, "count": n} for i, n in merged.items()]}
        if tools:
            row["tools"] = list(tools)
        self.rows.append(row)


def recipes(items: dict[str, dict]) -> list[dict]:
    b = Book(items)
    hammer, needle, knife, loupe = TOOLS["smithing"], TOOLS["tailoring"], TOOLS["fletching"], TOOLS["jewelcraft"]
    # Refining: one per tier and craft.
    for t in range(4):
        b.add("smithing", ORE[t][2], 1, [(ORE[t][0], 1), ("forge_coal", 1)], INTER_TRIVIAL[t])
        b.add("tailoring", FIBER[t][2], 1, [(FIBER[t][0], 2)], INTER_TRIVIAL[t])
        b.add("tailoring", HIDE[t][2], 1, [(HIDE[t][0], 1), ("tanning_salts", 1)], INTER_TRIVIAL[t] + 5)
        b.add("fletching", WOOD[t][2], 2, [(WOOD[t][0], 1)], INTER_TRIVIAL[t])
        b.add("jewelcraft", GEMS[t][2], 1, [(GEMS[t][0], 1)], INTER_TRIVIAL[t], (loupe,))
    b.add("tailoring", "bowstring", 1, [("spool_of_thread", 2)], 25)

    def bar(t): return ORE[t][2]
    def bolt(t): return FIBER[t][2]
    def leather(t): return HIDE[t][2]
    def shafts(t): return WOOD[t][2]
    def wood(t): return WOOD[t][0]
    def gem(t): return GEMS[t][2]

    # Armour: chain and plate at the forge, cloth and leather at the loom.
    for kind, skill in (("chain", "smithing"), ("plate", "smithing"), ("cloth", "tailoring"), ("leather", "tailoring")):
        for ti, lvl in enumerate(ARMOR_TIERS):
            t = tier_of(lvl)
            for slot in ARMOR_SLOTS:
                iid = f"{kind}_t{ti + 1}_{slot}"
                n = SLOT_COUNT[slot]
                extra = rarity_extra(items[iid]["rarity"])
                if skill == "smithing":
                    b.add(skill, iid, 1, [(bar(t), n + (1 if kind == "plate" else 0))] + extra, trivial_for(lvl) + (3 if kind == "plate" else 0), (hammer,))
                else:
                    main = bolt(t) if kind == "cloth" else leather(t)
                    b.add(skill, iid, 1, [(main, n), ("spool_of_thread", 1)] + extra, trivial_for(lvl), (needle,))
    # Shields at the forge.
    for key, n in SHIELD_BARS.items():
        for ti, lvl in enumerate(ARMOR_TIERS):
            t = tier_of(lvl)
            iid = f"{key}_t{ti + 1}"
            b.add("smithing", iid, 1, [(bar(t), n), (leather(t), 1)] + rarity_extra(items[iid]["rarity"]), trivial_for(lvl) + 2, (hammer,))
    # Weapons: metal at the forge, bows and staves at the fletching bench, handwraps at the loom.
    for ti, lvl in enumerate(WEAPON_TIERS):
        t = tier_of(lvl)
        for key, n in METAL_WEAPONS.items():
            iid = f"{key}_t{ti + 1}"
            b.add("smithing", iid, 1, [(bar(t), n), (leather(t), 1)] + rarity_extra(items[iid]["rarity"]), trivial_for(lvl) + 1, (hammer,))
        for key, n in (("shortbow", 2), ("longbow", 3)):
            iid = f"{key}_t{ti + 1}"
            b.add("fletching", iid, 1, [(wood(t), n), ("bowstring", 1)] + rarity_extra(items[iid]["rarity"]), trivial_for(lvl), (knife,))
        iid = f"staff_t{ti + 1}"
        b.add("fletching", iid, 1, [(wood(t), 3), (gem(t), 1)] + rarity_extra(items[iid]["rarity"]), trivial_for(lvl) + 2, (knife,))
        iid = f"handwraps_t{ti + 1}"
        b.add("tailoring", iid, 1, [(leather(t), 1), (bolt(t), 1)] + rarity_extra(items[iid]["rarity"]), trivial_for(lvl), (needle,))
    # Jewellery at the jeweler's bench; cloaks and shawls (the back slot) at the loom.
    for slot in ("neck", "ear", "finger", "face", "back"):
        for flavor in ("mystic", "martial"):
            for ti, lvl in enumerate(ARMOR_TIERS):
                t = tier_of(lvl)
                iid = f"{flavor}_{slot}_t{ti + 1}"
                extra = rarity_extra(items[iid]["rarity"])
                if slot == "back":
                    b.add("tailoring", iid, 1, [(bolt(t), 2), ("spool_of_thread", 1)] + extra, trivial_for(lvl) + 1, (needle,))
                else:
                    b.add("jewelcraft", iid, 1, [(bar(t), 1), (gem(t), 1)] + extra, trivial_for(lvl) + (2 if flavor == "martial" else 0), (loupe,))
    # Bags and bandages at the loom.
    for iid, ingredients, trivial in (
            ("cloth_pouch", [(bolt(0), 2), ("spool_of_thread", 1)], 25), ("small_sack", [(bolt(0), 4), ("spool_of_thread", 1)], 55),
            ("leather_satchel", [(leather(1), 3), ("spool_of_thread", 2)], 100),
            ("travelers_pack", [(bolt(1), 4), (leather(1), 2), ("spool_of_thread", 2)], 140),
            ("sturdy_backpack", [(leather(2), 4), (bolt(2), 2), ("spool_of_thread", 2), ("arcane_dust", 1)], 185),
            ("reinforced_haversack", [(leather(3), 4), (bar(2), 2), ("spool_of_thread", 2), ("arcane_dust", 2)], 230),
            ("bottomless_satchel", [(bolt(3), 4), (leather(3), 2), ("spool_of_thread", 3), ("ember_essence", 2)], 275)):
        b.add("tailoring", iid, 1, ingredients, trivial, (needle,))
    for iid, t in (("linen_bandage", 0), ("wool_bandage", 1), ("silk_bandage", 2), ("runeweave_bandage", 3)):
        b.add("tailoring", iid, 5, [(bolt(t), 1)], trivial_for(items[iid].get("req_level", 1)))
    # Arrows at the fletching bench: a bundle of shafts, a bar for the heads, feathers.
    for iid in ("rough_arrows", "fletched_arrows", "barbed_arrows", "steelhead_arrows", "runed_arrows", "starshot_arrows"):
        t = tier_of(items[iid]["req_level"])
        b.add("fletching", iid, 20, [(shafts(t), 1), (bar(t), 1), ("goose_feathers", 1)], trivial_for(items[iid]["req_level"]))
    # Cooking at the oven: steps for every stretch of skill.
    for iid, count, ingredients, trivial in (
            ("trail_rations", 4, [("raw_meat", 1), ("cooking_spices", 1)], 15), ("bread_dough", 2, [("flour_sack", 1), ("water_flask", 1)], 30),
            ("roast_haunch", 3, [("raw_meat", 2), ("cooking_spices", 1)], 58), ("meat_stock", 2, [("raw_meat", 1), ("water_flask", 1)], 80),
            ("hearty_stew", 3, [("meat_stock", 1), ("root_vegetables", 2)], 106), ("honey_glaze", 2, [("jar_of_honey", 1), ("cooking_spices", 1)], 140),
            ("pie_crust", 2, [("bread_dough", 1), ("flour_sack", 1)], 150),
            ("spiced_meat_pie", 3, [("prime_cut", 1), ("pie_crust", 1), ("cooking_spices", 1)], 178),
            ("smoked_prime_cut", 1, [("prime_cut", 1), ("forge_coal", 1)], 210),
            ("feast_loaf", 4, [("smoked_prime_cut", 1), ("bread_dough", 2), ("honey_glaze", 1), ("root_vegetables", 1)], 250)):
        b.add("cooking", iid, count, ingredients, trivial)
    # Brewing at the vat.
    herb = [h for h, _ in HERBS]
    for iid, count, ingredients in (
            ("spring_water", 2, [("water_flask", 2), (herb[0], 1)]), ("honeyed_tea", 2, [("water_flask", 1), ("jar_of_honey", 1), (herb[0], 1)]),
            ("clarity_draught", 2, [("water_flask", 1), (herb[2], 1), ("empty_vial", 1)]),
            ("moonwell_water", 2, [("water_flask", 1), (herb[3], 1), ("empty_vial", 1)]),
            ("minor_draught", 1, [(herb[0], 1), ("empty_vial", 1)]), ("lesser_draught", 1, [(herb[0], 2), ("empty_vial", 1)]),
            ("healing_draught", 1, [(herb[1], 1), ("empty_vial", 1)]), ("greater_draught", 1, [(herb[1], 2), ("empty_vial", 1)]),
            ("superior_draught", 1, [(herb[2], 2), ("empty_vial", 1)]), ("supreme_draught", 1, [(herb[3], 2), ("empty_vial", 1)]),
            ("minor_tonic", 1, [(herb[0], 1), ("water_flask", 1), ("empty_vial", 1)]),
            ("lesser_tonic", 1, [(herb[0], 2), ("water_flask", 1), ("empty_vial", 1)]),
            ("mana_tonic", 1, [(herb[1], 2), ("water_flask", 1), ("empty_vial", 1)]),
            ("greater_tonic", 1, [(herb[2], 2), ("water_flask", 1), ("empty_vial", 1)]),
            ("superior_tonic", 1, [(herb[3], 2), ("water_flask", 1), ("empty_vial", 1)]),
            ("elixir_of_might", 1, [(herb[1], 2), ("arcane_dust", 1), ("empty_vial", 1)]),
            ("elixir_of_fortitude", 1, [(herb[1], 2), ("arcane_dust", 1), ("empty_vial", 1)]),
            ("elixir_of_insight", 1, [(herb[1], 2), ("arcane_dust", 1), ("empty_vial", 1)]),
            ("elixir_of_grace", 1, [(herb[1], 2), ("arcane_dust", 1), ("empty_vial", 1)])):
        lvl = items[iid].get("req_level", 1)
        b.add("brewing", iid, count, ingredients, trivial_for(lvl) + (4 if iid.startswith("elixir") else 0))
    # The masterworks: each teaches its tradeskill up to the cap.
    top = RULES["max_skill"]
    b.add("smithing", "emberwrought_showblade", 1, [(bar(3), 6), (gem(3), 1), (leather(3), 1), ("ember_essence", 2)], top, (hammer,))
    b.add("tailoring", "starloom_tapestry", 1, [(bolt(3), 6), (leather(3), 2), ("spool_of_thread", 3), ("ember_essence", 2)], top, (needle,))
    b.add("cooking", "hearthmasters_banquet", 1, [("smoked_prime_cut", 2), ("feast_loaf", 2), ("honey_glaze", 2), ("cooking_spices", 2),
                                                  ("ember_essence", 1)], top)
    b.add("brewing", "vintage_ember_cordial", 1, [(herb[3], 4), ("jar_of_honey", 2), ("empty_vial", 1), ("ember_essence", 1)], top)
    b.add("fletching", "heartwood_totem", 1, [(wood(3), 4), (gem(3), 1), ("ember_essence", 2)], top, (knife,))
    b.add("jewelcraft", "starfire_mosaic", 1, [(gem(3), 4), (bar(3), 2), ("ember_essence", 2)], top, (loupe,))
    missing = sorted({m[0] for m in MASTERWORKS} - {r["id"] for r in b.rows})
    if missing:
        sys.exit(f"gen_crafts: masterworks without a recipe: {missing}")
    ids = [r["id"] for r in b.rows]
    dupes = sorted({x for x in ids if ids.count(x) > 1})
    if dupes:
        sys.exit(f"gen_crafts: two recipes make {dupes}")
    for r in b.rows:  # Phase 16: past the first band, a recipe is taught by a book
        vol = book_volume(r["trivial"])
        if vol > 1:
            r["book"] = book_id(r["tradeskill"], vol)
    return sorted(b.rows, key=lambda r: ([t[0] for t in TRADESKILLS].index(r["tradeskill"]), r["trivial"], r["id"]))


# ------------------------------------------------------------------------------------------- vendor arbitrage

def _buy(value: int, econ: dict) -> int:
    """The least a vendor charges (the best charisma): game/items.cpp vendor_buy_price."""
    return max(1, math.ceil(value * econ["min_buy"])) if value > 0 else 0


def _sell(value: int, econ: dict) -> int:
    """The most a vendor pays (the best charisma), always below its own price: game/items.cpp vendor_sell_price."""
    if value <= 0:
        return 0
    paid = max(1, math.floor(value * econ["max_sell"]))
    b = _buy(value, econ)
    return min(paid, b - 1) if b > 1 else 0


def vendor_profits(rows: list[dict], items: dict[str, dict], sold: set, econ: dict) -> list[tuple[str, float, int, list[str]]]:
    """Recipes whose materials can all be bought (directly, or made from bought things) and whose result a vendor
    buys back for more than they cost, at the best prices either way: (recipe, cost, paid back, the bought raw
    materials behind it). Combines are assumed to succeed, so the check is on the safe side.

    Each item's cheapest bought route is found by relaxing every recipe until nothing gets cheaper (every producer
    of an item counts, chains of any length, cycles end), then each recipe is costed from those routes."""
    best: dict[str, tuple[float, frozenset]] = {i: (float(_buy(items[i]["value"], econ)), frozenset([i])) for i in sold if i in items}

    def costed(r: dict) -> tuple[float, frozenset] | None:
        total, raws = 0.0, set()
        for ing in r["ingredients"]:
            b = best.get(ing["item"])
            if b is None:
                return None
            total += b[0] * ing["count"]
            raws |= b[1]
        return total, frozenset(raws)

    for _ in range(len(rows) + 1):
        changed = False
        for r in rows:
            c = costed(r)
            if c is None:
                continue
            each = c[0] / r["result"]["count"]
            cur = best.get(r["result"]["item"])
            if cur is None or each < cur[0] - 1e-9:
                best[r["result"]["item"]] = (each, c[1])
                changed = True
        if not changed:
            break
    out = []
    for r in rows:
        c = costed(r)
        paid = _sell(items[r["result"]["item"]]["value"], econ) * r["result"]["count"]
        if c is not None and paid > c[0]:
            out.append((r["id"], c[0], paid, sorted(c[1])))
    return out


def balance_values(items: dict[str, dict], sold: set, econ: dict) -> None:
    """Raises the values of the bought raw materials this file defines until no recipe made only from bought things
    sells back for more than it cost (no buy-craft-sell loop). Changes `items` in place; tools/gen_items.py calls it
    with the vendors' stock before writing items.json."""
    owned = {m["id"] for m in material_items()}
    dropped = {d[3] for d in DROPS} | {e[1] for e in ESSENCE_NAMED}
    for _ in range(60):
        bad = vendor_profits(recipes(items), items, sold, econ)
        if not bad:
            return
        factor: dict[str, float] = {}
        for _rid, total, paid, raws in bad:
            # Raise what only vendors sell first, so monster drops keep their worth.
            mine = [r for r in raws if r in owned and r not in dropped] or [r for r in raws if r in owned]
            for raw in mine:
                factor[raw] = max(factor.get(raw, 1.0), paid / max(1.0, total) * 1.1)
        if not factor:
            sys.exit(f"gen_crafts: {bad[0][0]} sells for more than its bought materials, none of which this file prices")
        for raw, f in factor.items():
            items[raw]["value"] = int(math.ceil(items[raw]["value"] * f))
    sys.exit("gen_crafts: material prices did not settle (vendor arbitrage remains)")


# ------------------------------------------------------------------------------------------- output

def render(rows: list[dict]) -> str:
    doc_comment = ("Tradeskills (generated by tools/gen_crafts.py). Each tradeskill has one station kind; station NPCs (role 'station') stand in the "
                   "cities' crafting yards. A recipe combines its ingredients at its tradeskill's station, with its tools in the pack (never used up). "
                   "trivial: the skill past which the recipe teaches nothing and nearly always succeeds. See docs/DECISIONS.md (Phase 13).")
    lines = ["{", '  "_comment": ' + json.dumps(doc_comment) + ",", '  "rules": ' + json.dumps(RULES) + ",", '  "tradeskills": [']
    ts = [{"id": tid, "name": name, "station": kind, "station_name": sname, "description": desc} for tid, name, kind, sname, _, desc in TRADESKILLS]
    lines += ["    " + json.dumps(t) + ("," if i < len(ts) - 1 else "") for i, t in enumerate(ts)]
    lines += ["  ],", '  "recipes": [']
    lines += ["    " + json.dumps(r) + ("," if i < len(rows) - 1 else "") for i, r in enumerate(rows)]
    lines += ["  ]", "}", ""]
    text = "\n".join(lines)
    json.loads(text)
    return text


def main() -> int:
    items = {i["id"]: i for i in json.loads((CONTENT / "items.json").read_text(encoding="utf-8"))["items"]}
    missing = [m["id"] for m in material_items() if m["id"] not in items]
    if missing:
        print(f"gen_crafts: items.json lacks the materials {missing[:5]}... (run python tools/gen_items.py first)")
        return 1
    rows = recipes(items)
    text = render(rows)
    if "--check" in sys.argv[1:]:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            print("gen_crafts: stale content/crafts.json (run python tools/gen_crafts.py)")
            return 1
        print("gen_crafts: content/crafts.json is up to date")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    by: dict[str, int] = {}
    for r in rows:
        by[r["tradeskill"]] = by.get(r["tradeskill"], 0) + 1
    print(f"wrote {len(rows)} recipes {by}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
