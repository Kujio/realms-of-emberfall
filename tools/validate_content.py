#!/usr/bin/env python3
"""Validate /content JSON: structure, id format, uniqueness, and cross-references.

Mirrors the server's startup validation (server/src/game/content.cpp) so content errors are
caught in CI before a server ever boots. Exit code 0 = valid.

Usage: python tools/validate_content.py [content_dir]
"""
from __future__ import annotations

import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parent))
import world_check  # noqa: E402
import progression_check  # noqa: E402
import social_check  # noqa: E402
import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_checks import validate_effects, validate_environments, validate_visuals  # noqa: E402  (models.json / appearance.json / environments.json)
ID_RE = re.compile(r"^[a-z0-9_]{1,32}$")


class Checker:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def obj(self, v, where: str) -> dict:
        if not isinstance(v, dict):
            self.err(f"{where}: must be an object")
            return {}
        return v

    def string(self, d: dict, key: str, where: str, required: bool = True) -> str:
        if key not in d:
            if required:
                self.err(f"{where}: missing '{key}'")
            return ""
        if not isinstance(d[key], str):
            self.err(f"{where}: '{key}' must be a string")
            return ""
        return d[key]

    def number(self, d: dict, key: str, where: str) -> float:
        v = d.get(key)
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
            self.err(f"{where}: '{key}' must be a finite number")
            return 0.0
        return float(v)

    def integer(self, d: dict, key: str, where: str) -> int:
        v = d.get(key)
        if isinstance(v, bool) or not isinstance(v, int):
            self.err(f"{where}: '{key}' must be an integer")
            return 0
        return v

    def str_list(self, d: dict, key: str, where: str) -> list[str]:
        v = d.get(key)
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
            self.err(f"{where}: '{key}' must be an array of strings")
            return []
        return v

    def spawn(self, d: dict, key: str, where: str) -> tuple[float, float]:
        s = self.obj(d.get(key), f"{where}.{key}")
        for k in ("x", "y", "z", "yaw"):
            self.number(s, k, f"{where}.{key}")
        return (float(s.get("x", 0) or 0), float(s.get("z", 0) or 0))

    def list_of(self, doc, key: str, file: str) -> list[dict]:
        if not isinstance(doc, dict) or not isinstance(doc.get(key), list):
            self.err(f"{file}: top-level '{key}' array is required")
            return []
        return [self.obj(e, f"{key}[{i}]") for i, e in enumerate(doc[key])]

    def ids(self, items: list[dict], kind: str) -> set[str]:
        seen: set[str] = set()
        for i, it in enumerate(items):
            iid = self.string(it, "id", f"{kind}[{i}]")
            name = self.string(it, "name", f"{kind}[{i}]")
            if iid and not ID_RE.match(iid):
                self.err(f"{kind} id '{iid}' must be 1-32 chars of [a-z0-9_]")
            if iid in seen:
                self.err(f"duplicate {kind} id '{iid}'")
            if not 1 <= len(name) <= 48:
                self.err(f"{kind} '{iid}': name must be 1-48 chars")
            seen.add(iid)
        return seen


def load(path: Path, c: Checker):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        c.err(f"missing file {path}")
    except json.JSONDecodeError as e:
        c.err(f"{path}: invalid JSON: {e}")
    return {}


def validate(content_dir: Path) -> list[str]:
    c = Checker()
    rules = c.obj(load(content_dir / "rules.json", c), "rules")
    races = c.list_of(load(content_dir / "races.json", c), "races", "races.json")
    classes = c.list_of(load(content_dir / "classes.json", c), "classes", "classes.json")
    cities = c.list_of(load(content_dir / "cities.json", c), "cities", "cities.json")
    zones = world_check.load_zones(content_dir, c)

    if rules:
        if not 1 <= c.integer(rules, "max_level", "rules") <= 255:
            c.err("rules.max_level must be 1..255")
        if not 1 <= c.integer(rules, "max_characters_per_account", "rules") <= 16:
            c.err("rules.max_characters_per_account must be 1..16")
        mv = c.obj(rules.get("movement"), "rules.movement")
        if mv:
            if not 0 < c.number(mv, "run_speed", "rules.movement") <= 50:
                c.err("rules.movement.run_speed must be in (0, 50]")
            c.integer(mv, "input_budget_max", "rules.movement")
            if c.number(mv, "desync_tolerance", "rules.movement") <= 0:
                c.err("rules.movement.desync_tolerance must be > 0")
            if c.number(mv, "teleport_tolerance", "rules.movement") < 1:
                c.err("rules.movement.teleport_tolerance must be >= 1")

    class_ids = c.ids(classes, "class")
    city_ids = c.ids(cities, "city")
    zone_ids = c.ids(zones, "zone")
    c.ids(races, "race")
    for k in ("races", "classes", "cities", "zones"):
        if not locals()[k]:
            c.err(f"at least one entry in {k} is required")

    for z in classes:
        c.string(z, "archetype", f"class '{z.get('id')}'")

    zone_bounds: dict[str, tuple[float, float, float, float]] = {}
    for z in zones:
        zid = z.get("id", "?")
        b = c.obj(z.get("bounds"), f"zone '{zid}'.bounds")
        mnx, mnz, mxx, mxz = (c.number(b, k, f"zone '{zid}'.bounds") for k in ("min_x", "min_z", "max_x", "max_z"))
        if not (mnx < mxx and mnz < mxz):
            c.err(f"zone '{zid}': bounds min must be < max")
        zone_bounds[zid] = (mnx, mnz, mxx, mxz)
        c.number(z, "ground_y", f"zone '{zid}'")
        if not 10 <= c.number(z, "interest_radius", f"zone '{zid}'") <= 500:
            c.err(f"zone '{zid}': interest_radius must be 10..500")
        if c.number(z, "say_radius", f"zone '{zid}'") <= 0:
            c.err(f"zone '{zid}': say_radius must be > 0")
        if not (isinstance(z.get("safe_spawn"), dict) and z["safe_spawn"].get("_dungeon")):
            sx, sz = c.spawn(z, "safe_spawn", f"zone '{zid}'")
            if not (mnx <= sx <= mxx and mnz <= sz <= mxz):
                c.err(f"zone '{zid}': safe_spawn is outside bounds")

    for city in cities:
        cid = city.get("id", "?")
        zid = c.string(city, "zone", f"city '{cid}'")
        sx, sz = c.spawn(city, "spawn", f"city '{cid}'")
        if zid not in zone_ids:
            c.err(f"city '{cid}': unknown zone '{zid}'")
        elif zid in zone_bounds:
            mnx, mnz, mxx, mxz = zone_bounds[zid]
            if not (mnx <= sx <= mxx and mnz <= sz <= mxz):
                c.err(f"city '{cid}': spawn is outside zone bounds")

    for r in races:
        rid = r.get("id", "?")
        allowed = c.str_list(r, "allowed_classes", f"race '{rid}'")
        rc = c.str_list(r, "cities", f"race '{rid}'")
        if not allowed:
            c.err(f"race '{rid}': allowed_classes must not be empty")
        if not rc:
            c.err(f"race '{rid}': cities must not be empty")
        if len(allowed) > 32 or len(rc) > 8:
            c.err(f"race '{rid}': too many classes (max 32) or cities (max 8)")
        for cl in allowed:
            if cl not in class_ids:
                c.err(f"race '{rid}': unknown class '{cl}'")
        for ci in rc:
            if ci not in city_ids:
                c.err(f"race '{rid}': unknown city '{ci}'")
    validate_combat(c, content_dir, rules, races, classes, zones)
    validate_visuals(c, content_dir, races, classes)
    validate_environments(c, content_dir, zones)
    validate_effects(c, content_dir, c.list_of(load(content_dir / "spells.json", c), "spells", "spells.json"),
                     c.list_of(load(content_dir / "abilities.json", c), "abilities", "abilities.json"))
    npcs = c.list_of(load(content_dir / "npcs.json", c), "npcs", "npcs.json")
    travel = c.list_of(load(content_dir / "travel.json", c), "points", "travel.json")
    ml = rules.get("max_level") if rules else None
    max_level = ml if isinstance(ml, int) and not isinstance(ml, bool) and 1 <= ml <= 255 else 60
    world_check.validate_world(c, zones, cities, npcs, travel, max_level)
    # Phase 6g: bands and the 1-60 path, factions and mob specials, quests.
    mobs = {m.get("id"): m for m in c.list_of(load(content_dir / "mobs.json", c), "mobs", "mobs.json")}
    items = {i.get("id"): i for i in c.list_of(load(content_dir / "items.json", c), "items", "items.json")}
    factions = c.list_of(load(content_dir / "factions.json", c), "factions", "factions.json")
    mob_spells = c.list_of(load(content_dir / "mob_spells.json", c), "spells", "mob_spells.json")
    quests = c.list_of(load(content_dir / "quests.json", c), "quests", "quests.json")
    progression_check.check_bands(c, zones, mobs, max_level)
    progression_check.check_mobs(c, mobs, mob_spells, factions, {r.get("id") for r in races})
    spawned = {mid for z in zones for sp in z.get("spawns", []) for mid in (sp.get("mob"), sp.get("rare")) if mid}
    progression_check.check_quests(c, quests, {n.get("id"): n for n in npcs}, mobs, items, {f.get("id") for f in factions},
                                   {cl.get("id") for cl in classes}, max_level, spawned)
    fac_ids = {f.get("id") for f in factions}
    for n in npcs:
        if n.get("faction") and n["faction"] not in fac_ids:
            c.err(f"npc '{n.get('id')}': unknown faction '{n['faction']}'")
    # Phase 7: groups, chat, the filter, guild ranks, friends and /who.
    social_check.validate_social(c, content_dir, rules, load)
    return c.errors


STAT_KEYS = ("str", "sta", "agi", "dex", "wis", "int", "cha")
ABILITY_TYPES = {"nuke", "heal", "dot", "hot", "buff", "debuff", "cc", "pet", "utility", "passive", "discipline"}
EFFECT_TYPES = {"damage", "stun", "threat", "taunt", "buff", "heal_pct", "restore_endurance_pct", "area_threat",
                "heal", "dot", "hot", "debuff", "slow", "snare", "root", "mez", "fear", "dispel", "resurrect", "teleport",
                "restore_mana_pct", "summon_pet", "charm", "illusion", "summon_item", "lifetap", "drain", "feign_death", "hide",
                "sneak", "threat_drop", "track", "pick_lock", "hp_to_mana"}
LASTING_EFFECTS = {"buff", "debuff", "slow", "snare", "root", "mez", "fear", "dot", "hot", "charm", "illusion", "drain"}
HARMFUL_EFFECTS = {"damage", "stun", "taunt", "dot", "debuff", "slow", "snare", "root", "mez", "fear", "charm", "lifetap", "drain"}
HELPFUL_EFFECTS = {"heal", "hot", "resurrect", "heal_pct", "restore_mana_pct", "restore_endurance_pct", "teleport", "summon_pet",
                   "summon_item", "illusion", "feign_death", "hide", "sneak", "track", "hp_to_mana"}
SELF_ONLY_EFFECTS = {"feign_death", "hide", "sneak", "track", "hp_to_mana"}
ARMOR_TYPES = ("cloth", "leather", "chain", "plate")
WEAPON_TYPES = {"1h_slash", "2h_slash", "1h_pierce", "2h_pierce", "1h_blunt", "2h_blunt", "hand_to_hand", "archery", "throwing", "shield"}
PET_VARIANTS = {"elemental", "undead", "spirit", "animated", "beast"}
BUFF_STATS = {"damage_taken_pct", "melee_damage_pct", "crit_chance_pct", "ac", "max_hp", "max_mana", "hp_regen", "mana_regen",
              "str", "sta", "agi", "dex", "wis", "int", "cha", "attack_speed_pct", "move_speed_pct", "resist_magic", "resist_fire",
              "resist_cold", "resist_poison", "resist_disease", "resist_all", "damage_shield", "threat_pct", "invisible"}
TARGETS = {"enemy", "self", "friendly", "group", "area", "corpse", "pet"}
RESISTS = {"none", "magic", "fire", "cold", "poison", "disease"}
LEVEL_BANDS = ((1, 15), (15, 30), (30, 45), (45, 60))
RULE_SECTIONS = {
    "xp": ("base", "level_exponent", "growth", "kill_base", "group_bonus_per_member", "toughness_weight", "toughness_min", "toughness_max"),
    "con": ("blue_divisor", "green_divisor"),
    "combat": ("gcd_seconds", "melee_range", "loot_range", "event_radius", "in_combat_seconds", "base_hit_chance",
               "min_hit_chance", "max_hit_chance", "crit_multiplier", "max_mitigation", "max_avoid_chance"),
    "death": ("xp_loss_fraction", "min_level_for_xp_loss", "player_corpse_seconds", "mob_corpse_seconds"),
    "mob_scaling": ("hp_base", "hp_per_level", "hp_quad", "max_hit_base", "max_hit_per_level", "max_hit_quad",
                    "ac_per_level", "skill_per_level", "chase_seconds", "flee_seconds", "aggro_scan_ticks",
                    "resist_base", "resist_per_level"),
    "spells": ("tick_seconds", "buff_slots", "debuff_slots", "spellbook_size", "memorize_seconds", "move_interrupt_distance",
               "range_slack", "damage_spread", "resist_scale_base", "resist_scale_per_level", "resist_per_level_diff",
               "min_resist", "max_resist", "partial_resist_min", "concentration_base", "concentration_skill_bonus",
               "concentration_skill_scale", "concentration_damage_scale", "min_concentration", "max_concentration",
               "root_break_chance", "heal_aggro_radius", "buff_threat_per_level", "rez_offer_seconds", "rez_hp_pct",
               "max_area_targets", "meditate_bonus"),
    "pets": ("follow_distance", "leash_distance", "assist_radius", "back_off_seconds", "charm_check_seconds", "charm_break_base",
             "charm_break_per_level", "charm_break_cha_scale", "min_charm_break", "max_charm_break"),
    "songs": ("pulse_seconds",),
    "stealth": ("sneak_speed_mult", "sneak_aggro_mult", "hide_base", "hide_per_skill", "feign_base", "feign_per_skill", "feign_per_level",
                "pick_base", "pick_per_skill", "pick_per_level", "min_chance", "max_chance"),
    "economy": ("buy_base", "buy_per_cha", "min_buy", "max_buy", "sell_base", "sell_per_cha", "min_sell", "max_sell", "buyback_slots",
                "npc_range", "bank_slots", "trade_range", "trade_invite_seconds", "auction_fee_pct", "auction_min_fee", "auction_hours",
                "auction_max_listings", "auction_page_size", "loot_roll_seconds", "round_robin_free_seconds", "loot_party_max"),
    "content": ("min_class_abilities", "max_level_gap", "min_items"),
}
ITEM_KINDS = {"weapon", "armor", "shield", "jewelry", "ammo", "consumable", "scroll", "bag", "loot", "quest", "tradeskill"}
WORN_KINDS = {"weapon", "armor", "shield", "jewelry", "ammo"}
ITEM_SLOTS = {"head", "face", "neck", "shoulders", "back", "chest", "arms", "wrist", "hands", "finger", "waist", "legs", "feet", "primary",
              "secondary", "ranged", "ammo", "ear"}
RARITIES = {"common", "uncommon", "rare", "epic"}
ARMOR_TYPES = ("cloth", "leather", "chain", "plate")
STAT_KEYS = {"str", "sta", "agi", "dex", "wis", "int", "cha"}
NPC_ROLES = {"trainer", "lockbox", "vendor", "banker", "auctioneer", "teleporter", "ferry", "binder", "questgiver"}


def check_ability(c: "Checker", a: dict, w: str, spell: bool, class_skills: dict, skill_ids: set, max_level: int,
                  city_ids: set, refs: dict | None = None) -> None:
    """One ability (abilities.json) or spell (spells.json); mirrors content.cpp parse_ability/check_effect."""
    cls = c.string(a, "class", w)
    lvl = c.integer(a, "level", w)
    typ = c.string(a, "type", w)
    if cls not in class_skills:
        c.err(f"{w}: unknown class '{cls}'")
    if not 1 <= lvl <= max_level:
        c.err(f"{w}: level must be 1..max_level")
    if typ not in ABILITY_TYPES:
        c.err(f"{w}: unknown type '{typ}'")
    skill = a.get("skill")
    if skill is not None:
        if skill not in skill_ids:
            c.err(f"{w}: unknown skill '{skill}'")
        elif cls in class_skills and skill not in class_skills[cls]:
            c.err(f"{w}: class '{cls}' lacks skill '{skill}'")
        elif typ == "passive" and cls in class_skills and class_skills[cls].get(skill) != lvl:
            c.err(f"{w}: passive level must match the class skill unlock level")
    if typ == "passive":
        if spell:
            c.err(f"{w}: spells cannot be passive")
        if skill is None:
            c.err(f"{w}: passive abilities must name a skill")
        return
    resource = c.string(a, "resource", w)
    song = a.get("song", False)
    if not isinstance(song, bool):
        c.err(f"{w}: 'song' must be true or false")
        song = False
    if resource not in ("none", "endurance", "mana"):
        c.err(f"{w}: resource must be none, endurance, or mana")
    if spell and not song and resource != "mana":
        c.err(f"{w}: spells cost mana")
    if song and not spell:
        c.err(f"{w}: songs are spells (spells.json)")
    if song and (resource != "none" or a.get("cost", 0) != 0 or a.get("cast_time", 0) != 0):
        c.err(f"{w}: songs have no cost and no cast time")
    pos = a.get("position", "")
    if pos and (pos != "behind" or a.get("target") != "enemy"):
        c.err(f"{w}: position must be 'behind' on an enemy-target ability")
    for k in ("cast_time", "cooldown", "range"):
        c.number(a, k, w)
    c.integer(a, "cost", w)
    if not (isinstance(a.get("cast_time"), (int, float)) and 0 <= a["cast_time"] <= 30):
        c.err(f"{w}: cast_time must be 0..30 s")
    if not isinstance(a.get("gcd"), bool):
        c.err(f"{w}: 'gcd' must be true or false")
    tgt = c.string(a, "target", w)
    if tgt not in TARGETS:
        c.err(f"{w}: target must be one of {sorted(TARGETS)}")
    if tgt in ("enemy", "corpse") and not (isinstance(a.get("range"), (int, float)) and a["range"] > 0):
        c.err(f"{w}: enemy and corpse targets need a range")
    if tgt == "area" and not (isinstance(a.get("radius"), (int, float)) and 0 < a["radius"] <= 100):
        c.err(f"{w}: area spells need a radius of (0, 100]")
    if a.get("resist", "none") not in RESISTS:
        c.err(f"{w}: resist must be one of {sorted(RESISTS)}")
    if "stack_group" in a and not (isinstance(a["stack_group"], str) and ID_RE.match(a["stack_group"])):
        c.err(f"{w}: stack_group must be 1-32 chars of [a-z0-9_]")
    rank = c.integer(a, "rank", w) if "rank" in a else 1
    if not 1 <= rank <= 100:
        c.err(f"{w}: rank must be 1..100")
    if len(a.get("description", "")) > 256:
        c.err(f"{w}: description must be at most 256 chars")
    effects = a.get("effects")
    if not isinstance(effects, list) or not effects:
        c.err(f"{w}: effects must be a non-empty array")
        return
    beneficial = tgt not in ("enemy", "area")
    has_rez = False
    refs = refs or {}
    kinds = {e.get("type") for e in effects if isinstance(e, dict)}
    if song and kinds & {"summon_pet", "charm", "summon_item", "resurrect"}:
        c.err(f"{w}: songs cannot resurrect, summon, or charm")
    if tgt == "pet" and not kinds & {"heal", "heal_pct", "hot", "buff"}:
        c.err(f"{w}: pet-target abilities heal or buff the pet")
    for i, e in enumerate(effects):
        we = f"{w}.effects[{i}]"
        e = c.obj(e, we)
        et = c.string(e, "type", we)
        if et not in EFFECT_TYPES:
            c.err(f"{we}: unknown effect type '{et}'")
            continue
        has_rez = has_rez or et == "resurrect"
        dur = e.get("duration", 0)
        # A supplied numeric field must be a number (checked, so "bad" is reported, never read as 0).
        for k in ("base", "per_level", "max", "value", "duration", "radius", "tick"):
            if k in e:
                c.number(e, k, we)
        num = lambda k: e.get(k, 0) if isinstance(e.get(k, 0), (int, float)) and not isinstance(e.get(k, 0), bool) else 0
        if et in ("buff", "debuff") and e.get("stat") not in BUFF_STATS:
            c.err(f"{we}: unknown buff stat")
        if et in LASTING_EFFECTS and not (isinstance(dur, (int, float)) and 0 < dur <= 86400):
            c.err(f"{we}: lasting effects need a duration of (0, 86400] s")
        if et == "stun" and not (isinstance(dur, (int, float)) and 0 < dur <= 30):
            c.err(f"{we}: stun duration must be (0, 30] s")
        if et == "area_threat" and num("radius") <= 0:
            c.err(f"{we}: area_threat radius must be > 0")
        if et in ("slow", "snare") and not 0 < num("value") <= 90:
            c.err(f"{we}: slow/snare value is a percentage in (0, 90]")
        if et in ("heal", "dot", "hot") and num("base") <= 0 and num("per_level") <= 0:
            c.err(f"{we}: heal/dot/hot needs a positive base or per_level")
        if et == "dispel" and not 1 <= num("value") <= 15:
            c.err(f"{we}: dispel value (count) must be 1..15")
        if et == "resurrect" and not 0 < num("value") <= 100:
            c.err(f"{we}: resurrect value (XP percent) must be (0, 100]")
        if et in ("restore_mana_pct", "restore_endurance_pct", "heal_pct") and not 0 < num("value") <= 100:
            c.err(f"{we}: restore/heal percentages must be (0, 100]")
        if et == "teleport":
            dest = e.get("destination", "")
            if dest not in ("safe_spot", "bind") and dest not in city_ids:
                c.err(f"{we}: unknown teleport destination '{dest}'")
        if beneficial and et in HARMFUL_EFFECTS:
            c.err(f"{we}: a beneficial spell cannot carry harmful effects")
        if not beneficial and et in HELPFUL_EFFECTS:
            c.err(f"{we}: a detrimental spell cannot heal, restore, teleport, summon, or resurrect")
        named = [k for k in ("pet", "item", "model") if k in e]
        if len(named) > 1:
            c.err(f"{we}: an effect names at most one of pet, item, or model")
        if et == "summon_pet":
            if e.get("pet") not in refs.get("pets", set()):
                c.err(f"{we}: unknown pet '{e.get('pet')}'")
            if num("base") <= 0 and num("per_level") <= 0:
                c.err(f"{we}: summon_pet needs a positive base or per_level (the pet's level)")
        if et == "summon_item":
            item = refs.get("items", {}).get(e.get("item"))
            if item is None:
                c.err(f"{we}: unknown item '{e.get('item')}'")
            elif not item.get("summoned"):
                c.err(f"{we}: item '{e.get('item')}' must be marked summoned")
            elif num("value") > item.get("stack", 1):
                c.err(f"{we}: summons more than one stack holds")
            if not 1 <= num("value") <= 100 or not 0 <= num("duration") <= 86400:
                c.err(f"{we}: summon_item needs a count (value) of 1..100 and a duration of 0..86400 s")
        if et == "illusion" and e.get("model") not in refs.get("looks", set()):
            c.err(f"{we}: illusion '{e.get('model')}' is neither a race nor a model")
        if et in ("lifetap", "drain", "hp_to_mana") and num("base") <= 0 and num("per_level") <= 0:
            c.err(f"{we}: {et} needs a positive base or per_level")
        if et in ("lifetap", "drain", "threat_drop") and not 0 <= num("value") <= 100:
            c.err(f"{we}: lifetap/drain/threat_drop value is a percentage in [0, 100]")
        if et == "threat_drop" and num("value") <= 0:
            c.err(f"{we}: threat_drop needs a value")
        if et in SELF_ONLY_EFFECTS and tgt != "self":
            c.err(f"{we}: {et} targets self")
        if et == "track" and num("radius") <= 0:
            c.err(f"{we}: track needs a radius")
        if et in ("charm", "pick_lock") and tgt != "enemy":
            c.err(f"{we}: charm and pick_lock need an enemy (targeted) ability")
        if tgt == "corpse" and et != "resurrect":
            c.err(f"{we}: a corpse-target spell can only resurrect")
    if has_rez != (tgt == "corpse"):
        c.err(f"{w}: corpse targets go with a resurrect effect, and only them")


def validate_combat(c: Checker, content_dir: Path, rules: dict, races: list[dict], classes: list[dict], zones: list[dict]) -> None:
    """Phase 2 data: stats, skills, abilities, items, loot tables, mobs, and spawns."""
    max_level = rules.get("max_level", 60) if isinstance(rules.get("max_level"), int) else 60
    integer_fields = {("economy", k) for k in ("buyback_slots", "bank_slots", "auction_min_fee", "auction_max_listings", "auction_page_size",
                                               "loot_party_max")} | {("content", k) for k in RULE_SECTIONS["content"]}
    for sec, keys in RULE_SECTIONS.items():
        d = c.obj(rules.get(sec), f"rules.{sec}")
        for k in keys:
            if (sec, k) in integer_fields:
                c.integer(d, k, f"rules.{sec}")
            else:
                c.number(d, k, f"rules.{sec}")
    xp = rules.get("xp") if isinstance(rules.get("xp"), dict) else {}
    cm = c.obj(xp.get("con_multipliers"), "rules.xp.con_multipliers")
    for k in ("grey", "green", "blue", "white", "yellow", "red"):
        if not 0 <= c.number(cm, k, "rules.xp.con_multipliers") <= 10:
            c.err(f"rules.xp.con_multipliers.{k} must be 0..10")
    if isinstance(xp.get("growth"), (int, float)) and not 1.0 <= xp["growth"] <= 1.5:
        c.err("rules.xp.growth must be in [1, 1.5]")
    try:
        total = sum(float(xp["base"]) * l ** float(xp["level_exponent"]) * float(xp["growth"]) ** (l - 1)
                    for l in range(1, min(int(max_level), 255)))
        if not math.isfinite(total) or total >= 1e15:
            c.err("rules.xp: total XP to max_level must stay below 1e15")
    except (KeyError, TypeError, ValueError, OverflowError):
        pass  # the field checks above already reported the problem

    for r in races:
        st = c.obj(r.get("stats"), f"race '{r.get('id')}'.stats")
        for k in STAT_KEYS:
            if not 1 <= c.integer(st, k, f"race '{r.get('id')}'.stats") <= 255:
                c.err(f"race '{r.get('id')}': stat {k} must be 1..255")

    skills_doc = load(content_dir / "skills.json", c)
    skills = c.list_of(skills_doc, "skills", "skills.json")
    skill_ids = c.ids(skills, "skill")
    defaults = c.obj(skills_doc.get("defaults") if isinstance(skills_doc, dict) else {}, "skills.defaults")

    def cap_ok(cap, where: str) -> int:
        cap = c.obj(cap, where)
        lvl = c.integer(cap, "level", where)
        c.number(cap, "per_level", where)
        mx = c.integer(cap, "max", where)
        if not 1 <= lvl <= max_level or not 1 <= mx <= 1000:
            c.err(f"{where}: invalid skill cap")
        return lvl

    for sid, cap in defaults.items():
        if sid not in skill_ids:
            c.err(f"skills.defaults: unknown skill '{sid}'")
        cap_ok(cap, f"skills.defaults.{sid}")

    class_skills: dict[str, dict[str, int]] = {}
    for cl in classes:
        cid = cl.get("id", "?")
        cb = c.obj(cl.get("combat"), f"class '{cid}'.combat")
        for k in ("hp_base", "hp_per_level", "mana_base", "mana_per_level", "endurance_base", "endurance_per_level",
                  "ac_per_level", "weapon_damage_base", "weapon_damage_per_level", "weapon_delay", "crit_chance"):
            c.number(cb, k, f"class '{cid}'.combat")
        if isinstance(cb.get("weapon_delay"), (int, float)) and not 0.5 <= cb["weapon_delay"] <= 10:
            c.err(f"class '{cid}': weapon_delay must be 0.5..10 s")
        levels = {sid: cap_ok(cap, f"skills.defaults.{sid}") for sid, cap in defaults.items()}
        for sid, cap in c.obj(cl.get("skills", {}), f"class '{cid}'.skills").items():
            if sid not in skill_ids:
                c.err(f"class '{cid}': unknown skill '{sid}'")
            levels[sid] = cap_ok(cap, f"class '{cid}'.skills.{sid}")
        for need in ("offense", "defense"):
            if need not in levels:
                c.err(f"class '{cid}': needs offense and defense skills")
        tm = cb.get("threat_mult", 1.0)
        if not isinstance(tm, (int, float)) or isinstance(tm, bool) or not 0 < tm <= 10:
            c.err(f"class '{cid}': threat_mult must be in (0, 10]")
        if cl.get("armor", "plate") not in ARMOR_TYPES:
            c.err(f"class '{cid}': armor must be cloth, leather, chain, or plate")
        for wpn in c.str_list(cl, "weapons", f"class '{cid}'") if "weapons" in cl else []:
            if wpn not in WEAPON_TYPES:
                c.err(f"class '{cid}': unknown weapon type '{wpn}'")
        class_skills[cid] = levels

    cities = c.list_of(load(content_dir / "cities.json", c), "cities", "cities.json")
    # Teleport destinations: cities and travel points (content/travel.json).
    city_ids = {x.get("id") for x in cities} | {t.get("id") for t in load(content_dir / "travel.json", c).get("points", [])}
    items = c.list_of(load(content_dir / "items.json", c), "items", "items.json")
    item_ids = c.ids(items, "item")
    pets = validate_pets(c, content_dir)
    models_doc = load(content_dir / "models.json", c)
    model_ids = {m.get("id") for m in (models_doc.get("models", []) if isinstance(models_doc, dict) else []) if isinstance(m, dict)}
    refs = {"pets": pets, "items": {it.get("id"): it for it in items if isinstance(it, dict)},
            "looks": model_ids | {r.get("id") for r in races}}
    abilities = c.list_of(load(content_dir / "abilities.json", c), "abilities", "abilities.json")
    ability_ids = c.ids(abilities, "ability")
    for a in abilities:
        check_ability(c, a, f"ability '{a.get('id', '?')}'", False, class_skills, skill_ids, max_level, city_ids, refs)
    spells = c.list_of(load(content_dir / "spells.json", c), "spells", "spells.json")
    spell_ids = c.ids(spells, "spell")
    for sp in spells:
        if sp.get("id") in ability_ids:
            c.err(f"spell '{sp.get('id')}': id is also an ability id")
        check_ability(c, sp, f"spell '{sp.get('id', '?')}'", True, class_skills, skill_ids, max_level, city_ids, refs)
    # Class shape: enough abilities, and something new at least every max_level_gap levels.
    shape = rules.get("content") if isinstance(rules.get("content"), dict) else {}
    need, gap = shape.get("min_class_abilities", 0), shape.get("max_level_gap", 0)
    for cl in classes:
        cid = cl.get("id")
        rows = [a for a in abilities + spells if isinstance(a, dict) and a.get("class") == cid]
        if isinstance(need, int) and need > 0 and len(rows) < need:
            c.err(f"class '{cid}': {len(rows)} abilities and spells, needs at least {need}")
        if isinstance(gap, int) and gap > 0:
            levels = sorted({a.get("level") for a in rows if isinstance(a.get("level"), int)})
            if 1 not in levels:
                c.err(f"class '{cid}': needs an ability at level 1")
            prev = 1
            for l in levels:
                if l - prev > gap:
                    c.err(f"class '{cid}': nothing new between levels {prev} and {l}")
                prev = l
            if max_level - prev > gap:
                c.err(f"class '{cid}': nothing new after level {prev}")
    stacks = {}
    for it in items:
        w = f"item '{it.get('id')}'"
        c.string(it, "kind", w)
        st = c.integer(it, "stack", w)
        stacks[it.get("id")] = st
        if not 1 <= st <= 1000:
            c.err(f"{w}: stack must be 1..1000")
        if c.integer(it, "value", w) < 0:
            c.err(f"{w}: value must be >= 0")
        if (it.get("kind") == "scroll") != bool(it.get("spell")):
            c.err(f"{w}: scrolls (and only scrolls) name a spell")
        if it.get("spell") and it["spell"] not in spell_ids:
            c.err(f"{w}: unknown spell '{it['spell']}'")
        if "summoned" in it and not isinstance(it["summoned"], bool):
            c.err(f"{w}: 'summoned' must be true or false")
        if it.get("summoned") and it.get("kind") == "scroll":
            c.err(f"{w}: scrolls cannot be summoned items")

    loot = c.list_of(load(content_dir / "loot.json", c), "loot_tables", "loot.json")
    loot_ids: set[str] = set()
    for t in loot:
        tid = c.string(t, "id", "loot table")
        if tid in loot_ids:
            c.err(f"duplicate loot table id '{tid}'")
        loot_ids.add(tid)
        w = f"loot table '{tid}'"
        lo, hi = c.integer(t, "coin_min", w), c.integer(t, "coin_max", w)
        if not 0 <= lo <= hi:
            c.err(f"{w}: need 0 <= coin_min <= coin_max")
        if not 0 <= c.integer(t, "max_drops", w) <= 16:
            c.err(f"{w}: max_drops must be 0..16")
        for d in t.get("drops", []) if isinstance(t.get("drops"), list) else []:
            d = c.obj(d, w)
            item = c.string(d, "item", w)
            if item not in item_ids:
                c.err(f"{w}: unknown item '{item}'")
            if not 0 <= c.number(d, "chance", w) <= 1:
                c.err(f"{w}: chance must be 0..1")
            mn, mx = c.integer(d, "min", w), c.integer(d, "max", w)
            if not 1 <= mn <= mx or (item in stacks and mx > stacks[item]):
                c.err(f"{w}: need 1 <= min <= max <= stack")
            if refs["items"].get(item, {}).get("summoned"):
                c.err(f"{w}: summoned item '{item}' cannot drop")

    mobs = c.list_of(load(content_dir / "mobs.json", c), "mobs", "mobs.json")
    mob_ids = c.ids(mobs, "mob")
    for m in mobs:
        w = f"mob '{m.get('id')}'"
        if not 1 <= c.integer(m, "level", w) <= max_level:
            c.err(f"{w}: level must be 1..max_level")
        c.string(m, "model", w)
        c.string(m, "faction", w)
        for k in ("scale", "hp_mult", "damage_mult", "attack_delay", "run_speed", "aggro_radius", "assist_radius",
                  "flee_hp_pct", "wander_radius", "leash_radius", "respawn_seconds"):
            c.number(m, k, w)
        for k in ("assist", "stun_immune"):
            if not isinstance(m.get(k), bool):
                c.err(f"{w}: '{k}' must be true or false")
        if "charm_immune" in m and not isinstance(m["charm_immune"], bool):
            c.err(f"{w}: 'charm_immune' must be true or false")
        if isinstance(m.get("leash_radius"), (int, float)) and m["leash_radius"] < 10:
            c.err(f"{w}: leash_radius must be >= 10")
        if isinstance(m.get("flee_hp_pct"), (int, float)) and not 0 <= m["flee_hp_pct"] < 1:
            c.err(f"{w}: flee_hp_pct must be in [0, 1)")
        if m.get("loot") and m["loot"] not in loot_ids:
            c.err(f"{w}: unknown loot table '{m['loot']}'")

    validate_spells(c, content_dir, rules, classes, zones, spells, items, loot, max_level)
    validate_items(c, content_dir, rules, classes, races, items, loot, max_level)

    for z in zones:
        zid = z.get("id", "?")
        b = z.get("bounds") if isinstance(z.get("bounds"), dict) else {}
        for i, sp in enumerate(z.get("spawns", []) if isinstance(z.get("spawns"), list) else []):
            sp = c.obj(sp, f"zone '{zid}'.spawns[{i}]")
            mob = c.string(sp, "mob", f"zone '{zid}'.spawns[{i}]")
            if mob not in mob_ids:
                c.err(f"zone '{zid}': spawn references unknown mob '{mob}'")
            if "room" in sp:  # placed in a dungeon room (world_check validates the room)
                if not 1 <= sp.get("count", 1) <= 20:
                    c.err(f"zone '{zid}': spawn count for '{mob}' must be 1..20")
                continue
            x, zz = c.number(sp, "x", f"zone '{zid}'.spawns[{i}]"), c.number(sp, "z", f"zone '{zid}'.spawns[{i}]")
            if b and not (b.get("min_x", 0) <= x <= b.get("max_x", 0) and b.get("min_z", 0) <= zz <= b.get("max_z", 0)):
                c.err(f"zone '{zid}': spawn for '{mob}' is outside bounds")
            if not 1 <= sp.get("count", 1) <= 20:
                c.err(f"zone '{zid}': spawn count for '{mob}' must be 1..20")


def validate_pets(c: Checker, content_dir: Path) -> set:
    """Phase 4: pet templates (content/pets.json). Returns the pet ids."""
    pets = c.list_of(load(content_dir / "pets.json", c), "pets", "pets.json")
    ids = c.ids(pets, "pet")
    models_doc = load(content_dir / "models.json", c)
    model_ids = {m.get("id") for m in (models_doc.get("models", []) if isinstance(models_doc, dict) else []) if isinstance(m, dict)}
    for pt in pets:
        w = f"pet '{pt.get('id')}'"
        if pt.get("variant") not in PET_VARIANTS:
            c.err(f"{w}: variant must be one of {sorted(PET_VARIANTS)}")
        if pt.get("model") not in model_ids:
            c.err(f"{w}: unknown model '{pt.get('model')}'")
        if len(pt.get("name", "")) > 24:
            c.err(f"{w}: name must be 1..24 chars")
        for k, lo, hi in (("scale", 0.1, 10), ("hp_mult", 1e-6, 100), ("damage_mult", 1e-6, 100), ("attack_delay", 0.5, 10),
                          ("run_speed", 1e-6, 20), ("threat_mult", 1e-6, 100), ("damage_shield_per_level", 0, 20)):
            if k in pt:
                v = c.number(pt, k, w)
                if not lo <= v <= hi:
                    c.err(f"{w}: {k} must be {lo:g}..{hi:g}")
        if "body" in pt and not (isinstance(pt["body"], str) and ID_RE.match(pt["body"])):
            c.err(f"{w}: body must be 1-32 chars of [a-z0-9_]")
        rs = pt.get("resists", {})
        for k, v in (rs.items() if isinstance(rs, dict) else []):
            if k not in RESISTS - {"none"} or not isinstance(v, int) or not 0 <= v <= 1000:
                c.err(f"{w}: resists must map magic/fire/cold/poison/disease to 0..1000")
    return ids


def validate_spells(c: Checker, content_dir: Path, rules: dict, classes: list[dict], zones: list[dict], spells: list[dict],
                    items: list[dict], loot: list[dict], max_level: int) -> None:
    """Phase 3: spell rules, trainers, scroll reachability, starter spells, and level-band coverage."""
    sr = rules.get("spells") if isinstance(rules.get("spells"), dict) else {}
    gems = sr.get("gem_unlock_levels")
    if not isinstance(gems, list) or not 1 <= len(gems) <= 8 or not all(isinstance(g, int) and not isinstance(g, bool) for g in gems):
        c.err("rules.spells.gem_unlock_levels needs 1..8 integer entries")
    elif any(not 1 <= g <= max_level for g in gems) or any(b < a for a, b in zip(gems, gems[1:])):
        c.err("rules.spells.gem_unlock_levels must be non-decreasing levels within 1..max_level")
    zone_by_id = {z.get("id"): z for z in zones}
    item_by_id = {it.get("id"): it for it in items}
    npcs = c.list_of(load(content_dir / "npcs.json", c), "npcs", "npcs.json")
    c.ids(npcs, "npc")
    class_ids = {cl.get("id") for cl in classes}
    spell_by_id = {s.get("id"): s for s in spells}
    for n in npcs:
        w = f"npc '{n.get('id')}'"
        if n.get("role") not in NPC_ROLES:
            c.err(f"{w}: role must be trainer, lockbox, vendor, banker, auctioneer, teleporter, ferry, or binder")
        z = zone_by_id.get(n.get("zone"))
        if n.get("role") in ("teleporter", "ferry", "binder", "questgiver"):  # travel and quests: tools/world_check.py
            if z is None:
                c.err(f"{w}: unknown zone '{n.get('zone')}'")
            if n.get("classes") or n.get("sells") or n.get("stock"):
                c.err(f"{w}: travel NPCs teach and sell nothing")
            continue
        if n.get("role") in ("vendor", "banker", "auctioneer"):
            if z is None:
                c.err(f"{w}: unknown zone '{n.get('zone')}'")
            else:
                b = z.get("bounds", {})
                if not (b.get("min_x", 0) <= n.get("x", 0) <= b.get("max_x", 0) and b.get("min_z", 0) <= n.get("z", 0) <= b.get("max_z", 0)):
                    c.err(f"{w}: position is outside the zone")
            if n.get("classes") or n.get("sells"):
                c.err(f"{w}: vendors list their goods in 'stock'; bankers and auctioneers sell nothing")
            continue
        if n.get("role") == "lockbox":
            ll = n.get("lock_level")
            if not isinstance(ll, int) or isinstance(ll, bool) or not 1 <= ll <= 255:
                c.err(f"{w}: a lockbox needs a lock_level of 1..255")
            if n.get("classes") or n.get("sells"):
                c.err(f"{w}: a lockbox teaches and sells nothing")
            if z is None:
                c.err(f"{w}: unknown zone '{n.get('zone')}'")
            continue
        if z is None:
            c.err(f"{w}: unknown zone '{n.get('zone')}'")
        else:
            b = z.get("bounds", {})
            if not (b.get("min_x", 0) <= n.get("x", 0) <= b.get("max_x", 0) and b.get("min_z", 0) <= n.get("z", 0) <= b.get("max_z", 0)):
                c.err(f"{w}: position is outside the zone")
        teaches = c.str_list(n, "classes", w)
        if not teaches:
            c.err(f"{w}: a trainer needs at least one class")
        for cl in teaches:
            if cl not in class_ids:
                c.err(f"{w}: unknown class '{cl}'")
        n["_sells"] = c.str_list(n, "sells", w) if "sells" in n else []
        for iid in n["_sells"]:
            it = item_by_id.get(iid)
            if it is None:
                c.err(f"{w}: sells unknown item '{iid}'")
            elif it.get("kind") == "scroll" and spell_by_id.get(it.get("spell"), {}).get("class") not in teaches:
                c.err(f"{w}: sells '{iid}' to a class it does not teach")
    scrolls_of: dict[str, list[str]] = {}
    for it in items:
        if it.get("kind") == "scroll":
            scrolls_of.setdefault(it.get("spell"), []).append(it.get("id"))
    sold = {(iid, cl) for n in npcs if n.get("zone") in zone_by_id for iid in n.get("_sells", []) for cl in n.get("classes", [])
            if isinstance(cl, str)}
    # Only tables that can award items count: used by a mob some zone spawns, with max_drops > 0,
    # and only drops whose chance is a valid positive number (bad ones were reported above).
    mobs = load(content_dir / "mobs.json", c)
    mob_loot = {m.get("id"): m.get("loot") for m in (mobs.get("mobs", []) if isinstance(mobs, dict) else []) if isinstance(m, dict)}
    live_tables = {mob_loot.get(sp.get("mob")) for z in zones for sp in (z.get("spawns") or []) if isinstance(sp, dict)}
    dropped = set()
    for t in loot:
        md = t.get("max_drops")
        if t.get("id") not in live_tables or not isinstance(md, int) or isinstance(md, bool) or md <= 0:
            continue
        for d in t.get("drops", []) if isinstance(t.get("drops"), list) else []:
            ch = d.get("chance") if isinstance(d, dict) else None
            if isinstance(ch, (int, float)) and not isinstance(ch, bool) and 0 < ch <= 1:
                dropped.add(d.get("item"))
        for pool in t.get("pools", []) if isinstance(t.get("pools"), list) else []:
            ch = pool.get("chance", 1.0) if isinstance(pool, dict) else None
            if isinstance(ch, (int, float)) and not isinstance(ch, bool) and 0 < ch <= 1:
                for e in pool.get("entries", []) if isinstance(pool.get("entries"), list) else []:
                    if isinstance(e, dict) and e.get("item"):
                        dropped.add(e.get("item"))
    for cl in classes:
        cl["_starters"] = c.str_list(cl, "starter_spells", f"class '{cl.get('id')}'") if "starter_spells" in cl else []
    starters = {cl.get("id"): cl["_starters"] for cl in classes}
    for s in spells:
        sid, cls = s.get("id"), s.get("class")
        reachable = sid in starters.get(cls, []) or any((sc, cls) in sold or sc in dropped for sc in scrolls_of.get(sid, []))
        if not reachable:
            c.err(f"spell '{sid}': not reachable (no starter grant, trainer, or loot drop teaches it)")
    level1_gems = sum(1 for g in gems if g == 1) if isinstance(gems, list) else 0
    book = sr.get("spellbook_size", 120)
    for cl in classes:
        cid = cl.get("id")
        distinct = set(cl["_starters"])
        if len(distinct) > level1_gems:
            c.err(f"class '{cid}': more starter spells than gems open at level 1")
        if isinstance(book, int) and len(distinct) > book:
            c.err(f"class '{cid}': starter spells do not fit the spellbook")
        for sid in cl["_starters"]:
            sp = spell_by_id.get(sid)
            if sp is None:
                c.err(f"class '{cid}': unknown starter spell '{sid}'")
            elif sp.get("class") != cid or sp.get("level") != 1:
                c.err(f"class '{cid}': starter spell '{sid}' must be its own level-1 spell")
    # Every class with any abilities or spells has some in each level band.
    abilities = c.list_of(load(content_dir / "abilities.json", c), "abilities", "abilities.json")
    for cl in classes:
        cid = cl.get("id")
        levels = [a.get("level", 0) for a in abilities + spells if a.get("class") == cid]
        if not levels:
            continue
        for lo, hi in LEVEL_BANDS:
            if lo <= max_level and not any(lo <= l <= hi for l in levels):
                c.err(f"class '{cid}': no abilities in levels {lo}-{hi}")


def validate_items(c: Checker, content_dir: Path, rules: dict, classes: list[dict], races: list[dict], items: list[dict], loot: list[dict],
                   max_level: int) -> None:
    """Phase 5: the item schema, item effects, loot pools, vendor stock, and starting kits (ids and references)."""
    fx_doc = load(content_dir / "item_effects.json", c)
    effects = c.list_of(fx_doc, "effects", "item_effects.json")
    fx_ids = c.ids(effects, "item effect")
    fx_by_id = {e.get("id"): e for e in effects}
    for e in effects:
        w = f"item effect '{e.get('id')}'"
        if e.get("class") != "item":
            c.err(f"{w}: class must be 'item'")
        if e.get("resource", "none") != "none":
            c.err(f"{w}: item effects use resource 'none'")
        if e.get("level") != 1:
            c.err(f"{w}: level must be 1")
        if e.get("target") not in TARGETS - {"corpse"}:
            c.err(f"{w}: unknown or unsupported target '{e.get('target')}'")
        if e.get("resist", "none") not in RESISTS:
            c.err(f"{w}: unknown resist")
        if not isinstance(e.get("effects"), list) or not e["effects"]:
            c.err(f"{w}: effects must be a non-empty array")
        for k in ("cost", "cast_time"):
            if e.get(k, 0) != 0:
                c.err(f"{w}: item effects have no {k} (the item sets the cast time)")
        # The full ability and effect checks (types, parameters, polarity), as the server applies them.
        check_ability(c, e, w, False, {"item": {}}, set(), max_level, set(), {"items": {it.get("id"): it for it in items}, "pets": set(),
                                                                              "looks": set()})
    item_ids = {it.get("id") for it in items}
    class_ids = {cl.get("id") for cl in classes}
    race_ids = {r.get("id") for r in races}
    weapons_of = {cl.get("id"): set(cl.get("weapons", [])) for cl in classes}
    non_scroll = 0
    for it in items:
        w = f"item '{it.get('id')}'"
        kind = it.get("kind")
        if kind != "scroll":
            non_scroll += 1
        if kind not in ITEM_KINDS:
            c.err(f"{w}: unknown kind '{kind}'")
        if it.get("rarity", "common") not in RARITIES:
            c.err(f"{w}: rarity must be common, uncommon, rare, or epic")
        name = it.get("name")
        if not isinstance(name, str) or not 1 <= len(name) <= 48:
            c.err(f"{w}: name must be 1-48 chars")
        if len(it.get("description", "")) > 256:
            c.err(f"{w}: description must be at most 256 chars")
        slots = it.get("slots", [])
        if not isinstance(slots, list) or any(sl not in ITEM_SLOTS for sl in slots):
            c.err(f"{w}: unknown slot in {slots}")
            slots = []
        if (kind in WORN_KINDS) != bool(slots):
            c.err(f"{w}: weapons, armor, shields, jewelry and ammo (and only they) have slots")
        if kind == "armor" and it.get("armor") not in ARMOR_TYPES:
            c.err(f"{w}: armor needs an armor type")
        if kind in ("weapon", "shield") and it.get("weapon") not in WEAPON_TYPES:
            c.err(f"{w}: weapons and shields need a weapon type")
        if kind == "weapon":
            dmg, dly = it.get("damage"), it.get("delay")
            if not isinstance(dmg, int) or not 1 <= dmg <= 1000 or not isinstance(dly, (int, float)) or not 0.5 <= dly <= 10:
                c.err(f"{w}: weapons need damage 1..1000 and delay 0.5..10 s")
            if str(it.get("weapon", "")).startswith("2h") and "secondary" in slots:
                c.err(f"{w}: two-handed weapons cannot go in the secondary slot")
        if kind == "shield" and slots != ["secondary"]:
            c.err(f"{w}: shields go in the secondary slot only")
        stats = it.get("stats", {})
        if not isinstance(stats, dict) or any(k not in STAT_KEYS or not isinstance(v, int) or not -100 <= v <= 100 for k, v in stats.items()):
            c.err(f"{w}: stats must map str/sta/agi/dex/wis/int/cha to -100..100")
        res = it.get("resists", {})
        if not isinstance(res, dict) or any(k not in RESISTS - {"none"} for k in res):
            c.err(f"{w}: unknown resist in {res}")
        if not 0 <= it.get("haste", 0) <= 100:
            c.err(f"{w}: haste must be 0..100")
        for key in ("proc", "click"):
            ref = it.get(key)
            if ref is None:
                continue
            if not isinstance(ref, dict) or ref.get("effect") not in fx_ids:
                c.err(f"{w}: {key} names an unknown item effect")
            elif key == "proc" and (fx_by_id[ref["effect"]].get("target") not in ("enemy", "area") or not 0 < ref.get("rate", 0) <= 0.5):
                c.err(f"{w}: a proc is an enemy effect with a rate in (0, 0.5]")
        if kind == "consumable" and not it.get("click"):
            c.err(f"{w}: consumables need a click effect")
        if (kind == "bag") != bool(it.get("bag_slots")):
            c.err(f"{w}: bags (and only bags) have bag_slots")
        elif kind == "bag" and not 1 <= it["bag_slots"] <= 16:
            c.err(f"{w}: bag_slots must be 1..16")
        for cl in it.get("classes", []):
            if cl not in class_ids:
                c.err(f"{w}: unknown class '{cl}'")
        for r in it.get("races", []):
            if r not in race_ids:
                c.err(f"{w}: unknown race '{r}'")
        if not 0 <= it.get("req_level", 0) <= max_level:
            c.err(f"{w}: req_level must be 0..max_level")
        if it.get("visual") and kind not in WORN_KINDS:
            c.err(f"{w}: only worn items have a visual")
    by_id_all = {it.get("id"): it for it in items}
    need = (rules.get("content") or {}).get("min_items", 0)
    if isinstance(need, int) and non_scroll < need:
        c.err(f"items: {non_scroll} items besides spell scrolls, needs at least {need}")
    for t in loot:
        w = f"loot table '{t.get('id')}'"
        for pool in t.get("pools", []) if isinstance(t.get("pools"), list) else []:
            entries = pool.get("entries") if isinstance(pool, dict) else None
            if not isinstance(entries, list) or not entries:
                c.err(f"{w}: a pool needs entries")
                continue
            if not 1 <= pool.get("rolls", 1) <= 8 or not 0 <= pool.get("chance", 1.0) <= 1:
                c.err(f"{w}: pool rolls must be 1..8 and chance 0..1")
            for e in entries:
                if not isinstance(e, dict) or not isinstance(e.get("weight"), (int, float)) or e["weight"] <= 0:
                    c.err(f"{w}: pool entries need a positive weight")
                elif e.get("item") and e["item"] not in item_ids:
                    c.err(f"{w}: unknown item '{e['item']}'")
                elif e.get("item"):
                    mn, mx = e.get("min", 1), e.get("max", e.get("min", 1))
                    if not (isinstance(mn, int) and isinstance(mx, int) and 1 <= mn <= mx <= by_id_all[e["item"]].get("stack", 1)):
                        c.err(f"{w}: pool entry for '{e['item']}' needs 1 <= min <= max <= stack")
                elif e.get("item") and by_id_all.get(e["item"], {}).get("summoned"):
                    c.err(f"{w}: summoned item '{e['item']}' cannot drop")
    npcs = c.list_of(load(content_dir / "npcs.json", c), "npcs", "npcs.json")
    by_id = {it.get("id"): it for it in items}
    for n in npcs:
        if n.get("role") != "vendor":
            if n.get("stock"):
                c.err(f"npc '{n.get('id')}': only vendors have stock")
            continue
        w = f"npc '{n.get('id')}'"
        stock = n.get("stock")
        if not isinstance(stock, list) or not 1 <= len(stock) <= 128:
            c.err(f"{w}: a vendor needs 1..128 stock lines")
            continue
        for line in stock:
            it = by_id.get(line.get("item")) if isinstance(line, dict) else None
            if it is None:
                c.err(f"{w}: stocks unknown item '{line}'")
                continue
            if it.get("summoned") or not it.get("value"):
                c.err(f"{w}: cannot stock '{it.get('id')}' (summoned or worthless)")
            count = line.get("count", -1)
            if isinstance(count, bool) or not isinstance(count, int) or not (count == -1 or 1 <= count <= 1000):
                c.err(f"{w}: stock count for '{it.get('id')}' must be -1 (unlimited) or 1..1000")
            elif count > 0 and not (isinstance(line.get("restock"), (int, float)) and 1 <= line["restock"] <= 86400):
                c.err(f"{w}: limited stock of '{it.get('id')}' needs a restock time of 1..86400 s")
    for cl in classes:
        kit = cl.get("starting_items", [])
        if not isinstance(kit, list):
            c.err(f"class '{cl.get('id')}': starting_items must be an array")
            continue
        for si in kit:
            if not isinstance(si, dict):
                c.err(f"class '{cl.get('id')}': each starting item must be an object")
                continue
            it = by_id.get(si.get("item"))
            if it is None:
                c.err(f"class '{cl.get('id')}': unknown starting item '{si.get('item')}'")
            elif si.get("equip") and it.get("kind") in ("weapon", "shield") and it.get("weapon") not in weapons_of.get(cl.get("id"), set()):
                c.err(f"class '{cl.get('id')}': starting item '{it['id']}' is a weapon the class cannot use")


def class_coverage(content_dir: Path) -> list[str]:
    """Informational: classes with no abilities yet (filled in by Phase 4)."""
    classes = json.loads((content_dir / "classes.json").read_text(encoding="utf-8")).get("classes", [])
    owned = set()
    for f in ("abilities.json", "spells.json"):
        doc = json.loads((content_dir / f).read_text(encoding="utf-8"))
        owned |= {a.get("class") for a in doc.get(f.split(".")[0], [])}
    return [cl["id"] for cl in classes if cl.get("id") not in owned]


SELF_TESTS = [
    # (file, mutation on the parsed JSON, an error message the validator must report)
    ("zones/emberglade.json", lambda d: d["zone_lines"].__setitem__(0, {**d["zone_lines"][0], "to": "nowhere"}), "leads to unknown zone 'nowhere'"),
    ("zones/emberglade.json", lambda d: d["spawns"].append({"mob": "the_emberwyrm", "x": 0, "z": 0, "count": 1}), "outside the band"),
    ([("zones/stormcrown_peaks.json", lambda d: d.__setitem__("levels", {"min": 50, "max": 57}) or d.__setitem__("spawns", [])),
      ("zones/emberwyrm_lair.json", lambda d: d.__setitem__("levels", {"min": 55, "max": 57}) or d.__setitem__("spawns", []))],
     None, "a dead end in the 1-60 path"),
    ("zones/saltmarsh_fens.json", lambda d: d.__setitem__("zone_lines", [l for l in d["zone_lines"] if l["to"] != "duskmere_shore"]),
     "cannot be reached from any starting city"),
    ("quests.json", lambda d: d["quests"][0]["steps"][0].__setitem__("mobs", ["no_such_mob"]), "kill step names unknown mob 'no_such_mob'"),
    ("quests.json", lambda d: d["quests"][0].__setitem__("prereqs", [d["quests"][1]["id"]]) or d["quests"][1].__setitem__("prereqs", [d["quests"][0]["id"]]),
     "prerequisites loop"),
    ("quests.json", lambda d: d["quests"][0].__setitem__("giver", "banker_odell"), "must have role questgiver"),
    ("quests.json", lambda d: d.__setitem__("quests", [q for q in d["quests"] if q.get("classes") != ["monk"]]), "class 'monk': no epic quest chain"),
    ("loot.json", lambda d: d["loot_tables"][0]["drops"].append({"item": "no_such_item", "chance": 0.5}), "no_such_item"),
    ("npcs.json", lambda d: next(n for n in d["npcs"] if n["role"] == "vendor")["stock"].append({"item": "no_such_item"}), "no_such_item"),
    ("mobs.json", lambda d: next(m for m in d["mobs"] if m.get("specials"))["specials"].append({"type": "cast", "effect": "nope", "every": 5}),
     "unknown mob spell 'nope'"),
    ("mobs.json", lambda d: d["mobs"][0].__setitem__("faction", "nobody"), "unknown faction 'nobody'"),
    ("social.json", lambda d: d["group"].__setitem__("max_members", 7), "max_members must be 2..6"),
    ("social.json", lambda d: d["guild"]["ranks"][1]["permissions"].append("disband"), "only the leader may disband"),
    ("social.json", lambda d: d["guild"]["ranks"][0]["permissions"].remove("motd"), "must hold every permission"),
    ("social.json", lambda d: d["chat"]["channels"].pop("ooc"), "channels.ooc: missing"),
    ("social.json", lambda d: d["filter"]["words"].append("two words"), "must be 1-32 ASCII letters"),
    ("social.json", lambda d: d.__setitem__("group", {}), "social.group: 'max_members' must be an integer"),
]


def self_test() -> int:
    """Breaks a copy of the content in each SELF_TESTS way and checks the validator notices."""
    import shutil
    import tempfile
    failures = 0
    for rel, mutate, expect in SELF_TESTS:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "content"
            shutil.copytree(ROOT / "content", copy)
            for r2, m2 in (rel if isinstance(rel, list) else [(rel, mutate)]):
                path = copy / r2
                doc = json.loads(path.read_text(encoding="utf-8"))
                m2(doc)
                path.write_text(json.dumps(doc), encoding="utf-8")
            errors = validate(copy)
            if not any(expect in e for e in errors):
                failures += 1
                print(f"self-test FAILED: breaking {rel} should report '{expect}'; got: {errors[:3]}", file=sys.stderr)
    if failures:
        return 1
    print(f"validator self-test: {len(SELF_TESTS)} broken copies, every one caught")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
    content_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "content"
    errors = validate(content_dir)
    if errors:
        print(f"content validation failed ({len(errors)} problem{'s' if len(errors) != 1 else ''}):", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"content OK ({content_dir})")
    pending = class_coverage(content_dir)
    if pending:
        print(f"note: no abilities yet for {len(pending)} class(es): {', '.join(pending)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
