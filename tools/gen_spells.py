#!/usr/bin/env python3
"""Generates the Cleric and Wizard spell lines (Phase 3) from tiered templates.

Writes, all committed and hand-editable:
  content/spells.json  every spell, one per line
  content/items.json   one scroll item per spell (other items are kept as they are)
  content/npcs.json    the class trainers that sell those scrolls

Numbers are derived from the same curves the server uses (content/rules.json mob_scaling and
classes.json), so a rank learned at level L is sized against an even-level mob at L. Each rank
grows with the caster's level until it reaches (roughly) the next rank's power, then caps.
Usage: python tools/gen_spells.py
"""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
MAX_LEVEL = 60
ZONE = "proving_flats"


def mob_hp(level: int) -> float:
    """Even-con mob HP before per-mob multipliers (rules.json mob_scaling)."""
    rules = json.loads((CONTENT / "rules.json").read_text(encoding="utf-8"))["mob_scaling"]
    return rules["hp_base"] + rules["hp_per_level"] * level + rules["hp_quad"] * level * level


def class_hp(class_id: str, level: int) -> float:
    classes = json.loads((CONTENT / "classes.json").read_text(encoding="utf-8"))["classes"]
    c = next(c for c in classes if c["id"] == class_id)["combat"]
    return c["hp_base"] + c["hp_per_level"] * level


def scaled(d0: float, d1: float, lvl: int, nxt: int) -> dict:
    """base/per_level/max so the amount is d0 at lvl and reaches d1 (then caps) at nxt."""
    span = max(1, nxt - lvl)
    per = max(0.0, (d1 - d0) / span)
    return {"base": round(d0 - per * lvl, 1), "per_level": round(per, 2), "max": round(max(d0, d1))}


def ladder(levels: list[int]) -> list[tuple[int, int, int]]:
    """(rank, level, next rank level) for a tiered line."""
    out = []
    for i, lvl in enumerate(levels):
        nxt = levels[i + 1] if i + 1 < len(levels) else MAX_LEVEL
        out.append((i + 1, lvl, nxt))
    return out


def spell(sid, name, cls, lvl, typ, cost, cast, recast, rng, target, resist, effects, desc, **extra):
    row = {"id": sid, "name": name, "class": cls, "level": lvl, "type": typ, "resource": "mana", "cost": int(round(cost)),
           "cast_time": round(cast, 2), "cooldown": recast, "gcd": True, "range": rng, "target": target, "resist": resist}
    row.update(extra)
    row["effects"] = effects
    row["description"] = desc
    return row


# ------------------------------------------------------------------------------------------- wizard

def wizard() -> list[dict]:
    W = "wizard"
    out: list[dict] = []
    eff = lambda lvl: 1.3 + 0.01 * lvl  # damage per mana at the rank's level

    fire = [("ember_dart", "Ember Dart", 1), ("flame_lash", "Flame Lash", 9), ("searing_bolt", "Searing Bolt", 18),
            ("pyre_lance", "Pyre Lance", 28), ("inferno_spike", "Inferno Spike", 38), ("sunflare", "Sunflare", 49),
            ("wrath_of_the_forge", "Wrath of the Forge", 59)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([f[2] for f in fire]), fire):
        d0, d1 = 0.38 * mob_hp(lvl), 0.36 * mob_hp(nxt)
        out.append(spell(sid, name, W, lvl, "nuke", d0 / eff(lvl), 1.75 + 0.2 * (rank - 1), 0, 60, "enemy", "fire",
                         [{"type": "damage", **scaled(d0, d1, lvl, nxt)}],
                         f"Hurl searing fire at your target. Rank {rank} of the fire line.", stack_group="wizard_fire", rank=rank))

    cold = [("frost_needle", "Frost Needle", 1), ("rime_shard", "Rime Shard", 14), ("glacial_spear", "Glacial Spear", 26),
            ("winters_fang", "Winter's Fang", 41)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([c[2] for c in cold]), cold):
        d0, d1 = 0.30 * mob_hp(lvl), 0.28 * mob_hp(nxt)
        snare = 25 + 5 * rank
        out.append(spell(sid, name, W, lvl, "nuke", d0 / (eff(lvl) - 0.15), 2.0 + 0.2 * (rank - 1), 0, 60, "enemy", "cold",
                         [{"type": "damage", **scaled(d0, d1, lvl, nxt)}, {"type": "snare", "value": snare, "duration": 12}],
                         f"A shard of frost that wounds and slows your target's movement by {snare}% for 12 seconds.",
                         stack_group="wizard_cold", rank=rank))

    magic = [("arcane_spark", "Arcane Spark", 12), ("arcane_torrent", "Arcane Torrent", 30), ("eldritch_cascade", "Eldritch Cascade", 56)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([m[2] for m in magic]), magic):
        d0, d1 = 0.55 * mob_hp(lvl), 0.5 * mob_hp(nxt)
        out.append(spell(sid, name, W, lvl, "nuke", d0 / (eff(lvl) + 0.2), 4.0, 6, 60, "enemy", "magic",
                         [{"type": "damage", **scaled(d0, d1, lvl, nxt)}],
                         "A slow, devastating torrent of raw arcane force. Efficient, but long to cast.", stack_group="wizard_arcane", rank=rank))

    pbae = [("cinder_ring", "Cinder Ring", 15), ("firestorm_halo", "Firestorm Halo", 32), ("pyroclasm", "Pyroclasm", 53)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([p[2] for p in pbae]), pbae):
        d0, d1 = 0.25 * mob_hp(lvl), 0.24 * mob_hp(nxt)
        out.append(spell(sid, name, W, lvl, "nuke", d0 / 0.8, 2.5, 8, 0, "area", "fire",
                         [{"type": "damage", **scaled(d0, d1, lvl, nxt)}],
                         "Erupt in a ring of flame that burns every foe within 12 meters of you.", stack_group="wizard_pbae", rank=rank,
                         radius=12))

    d0 = 0.28 * mob_hp(36)
    out.append(spell("hailstorm", "Hailstorm", W, 36, "nuke", d0 / 0.85, 3.0, 10, 60, "area", "cold",
                     [{"type": "damage", **scaled(d0, 0.27 * mob_hp(60), 36, 60)}, {"type": "snare", "value": 30, "duration": 8}],
                     "Call down hail on your target and every foe within 10 meters of it, slowing them briefly.", radius=10))

    for rank, (sid, name, lvl, dur, cap) in enumerate([("binding_frost", "Binding Frost", 6, 18, 30), ("glacial_chains", "Glacial Chains", 34, 30, 62)], 1):
        out.append(spell(sid, name, W, lvl, "cc", 10 + lvl * 1.5, 1.5, 0, 60, "enemy", "cold",
                         [{"type": "root", "duration": dur, "max_target_level": cap}],
                         f"Freeze your target's feet to the ground for {dur} seconds. Damage may break it free.",
                         stack_group="wizard_root", rank=rank))

    total = 0.6 * mob_hp(10)
    out.append(spell("smoldering_brand", "Smoldering Brand", W, 10, "dot", total / 2.0, 2.0, 0, 60, "enemy", "fire",
                     [{"type": "dot", **scaled(total / 6, 0.6 * mob_hp(30) / 6, 10, 30), "duration": 18, "tick": 3}],
                     "Brand your target with lingering fire that burns every 3 seconds for 18 seconds."))

    out.append(spell("arcane_frailty", "Arcane Frailty", W, 20, "debuff", 40, 2.0, 0, 60, "enemy", "magic",
                     [{"type": "debuff", "stat": "resist_all", "value": -20, "duration": 60},
                      {"type": "debuff", "stat": "ac", "value": -15, "duration": 60}],
                     "Unravel your target's defenses: lower its resistances and armor for one minute."))

    wards = [("ward_of_glass", "Ward of Glass", 1, 3, 8), ("mantle_of_runes", "Mantle of Runes", 22, 10, 45),
             ("aegis_of_the_magus", "Aegis of the Magus", 45, 20, 110)]
    for rank, (sid, name, lvl, ac, hp) in enumerate(wards, 1):
        out.append(spell(sid, name, W, lvl, "buff", 8 + lvl * 2, 2.5, 0, 0, "self", "none",
                         [{"type": "buff", "stat": "ac", "value": ac, "duration": 1800},
                          {"type": "buff", "stat": "max_hp", "value": hp, "duration": 1800}],
                         f"Wrap yourself in a shimmering ward: +{ac} armor and +{hp} health for 30 minutes.",
                         stack_group="wizard_ward", rank=rank))

    out.append(spell("homeward_step", "Homeward Step", W, 7, "utility", 25, 5.0, 60, 0, "self", "none",
                     [{"type": "teleport", "destination": "bind"}], "Step through the weave back to your bind point."))
    out.append(spell("wind_retreat", "Wind Retreat", W, 18, "utility", 60, 5.0, 300, 0, "group", "none",
                     [{"type": "teleport", "destination": "safe_spot"}],
                     "Evacuate: whisk yourself (and, once groups exist, your group) to the safe heart of this zone. Pursuers lose your trail."))
    out.append(spell("emberhold_translocation", "Emberhold Translocation", W, 24, "utility", 50, 6.0, 120, 0, "self", "none",
                     [{"type": "teleport", "destination": "emberhold"}],
                     "Open a fold in the world to the forge-city of Emberhold. Portals to other zones arrive with zone travel."))
    out.append(spell("gather_aether", "Gather Aether", W, 25, "utility", 0, 4.0, 1200, 0, "self", "none",
                     [{"type": "restore_mana_pct", "value": 60}], "Draw ambient magic into yourself, restoring 60% of your mana. 20 minute recast."))
    out.append(spell("deep_aether", "Deep Aether", W, 50, "utility", 0, 4.0, 900, 0, "self", "none",
                     [{"type": "restore_mana_pct", "value": 100}], "Drink deeply of the weave, restoring all of your mana. 15 minute recast.",
                     stack_group="gather_aether", rank=2))
    return out


# ------------------------------------------------------------------------------------------- cleric

def cleric() -> list[dict]:
    C = "cleric"
    out: list[dict] = []

    heals = [("minor_mending", "Minor Mending", 1), ("mending_light", "Mending Light", 5), ("restoring_light", "Restoring Light", 12),
             ("greater_restoration", "Greater Restoration", 20), ("radiant_renewal", "Radiant Renewal", 31),
             ("divine_restoration", "Divine Restoration", 44), ("sanctified_recovery", "Sanctified Recovery", 57)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([h[2] for h in heals]), heals):
        h0, h1 = 0.45 * class_hp("warrior", lvl), 0.42 * class_hp("warrior", nxt)
        out.append(spell(sid, name, C, lvl, "heal", h0 / (2.0 + 0.02 * lvl), 2.0 + 0.25 * (rank - 1), 0, 60, "friendly", "none",
                         [{"type": "heal", **scaled(h0, h1, lvl, nxt)}],
                         f"Mend the wounds of your target (or yourself). Rank {rank} of the healing line.", stack_group="cleric_heal", rank=rank))

    hots = [("soothing_glow", "Soothing Glow", 9), ("mending_aura", "Mending Aura", 28), ("everlight", "Everlight", 46)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([h[2] for h in hots]), hots):
        t0, t1 = 0.6 * class_hp("warrior", lvl), 0.55 * class_hp("warrior", nxt)
        out.append(spell(sid, name, C, lvl, "hot", t0 / 2.5, 2.5, 0, 60, "friendly", "none",
                         [{"type": "hot", **scaled(t0 / 8, t1 / 8, lvl, nxt), "duration": 24, "tick": 3}],
                         "Bathe your target in gentle light that heals every 3 seconds for 24 seconds.", stack_group="cleric_hot", rank=rank))

    wards = [("blessed_ward", "Blessed Ward", 1, 4, 10), ("sacred_armor", "Sacred Armor", 10, 9, 35), ("hallowed_bulwark", "Hallowed Bulwark", 22, 15, 80),
             ("bastion_of_light", "Bastion of Light", 36, 22, 150), ("aegis_of_devotion", "Aegis of Devotion", 50, 30, 240)]
    for rank, (sid, name, lvl, ac, hp) in enumerate(wards, 1):
        out.append(spell(sid, name, C, lvl, "buff", 10 + lvl * 2.5, 3.0, 0, 60, "friendly", "none",
                         [{"type": "buff", "stat": "ac", "value": ac, "duration": 1800},
                          {"type": "buff", "stat": "max_hp", "value": hp, "duration": 1800}],
                         f"Bless your target with +{ac} armor and +{hp} health for 30 minutes.", stack_group="cleric_ward", rank=rank))

    undead = [("ward_the_restless", "Ward the Restless", 4), ("banish_dead", "Banish Dead", 15), ("sunder_undeath", "Sunder Undeath", 33),
              ("lights_judgment", "Light's Judgment", 52)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([u[2] for u in undead]), undead):
        d0, d1 = 0.45 * mob_hp(lvl), 0.42 * mob_hp(nxt)
        out.append(spell(sid, name, C, lvl, "nuke", d0 / 2.0, 2.5, 0, 60, "enemy", "magic",
                         [{"type": "damage", **scaled(d0, d1, lvl, nxt)}],
                         "Searing holy light that unmakes the walking dead. Only affects the undead.",
                         stack_group="cleric_undead", rank=rank, requires_body="undead", resist_adjust=-30))

    smites = [("holy_rebuke", "Holy Rebuke", 1), ("radiant_smite", "Radiant Smite", 27), ("wrath_of_the_pure", "Wrath of the Pure", 49)]
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([s[2] for s in smites]), smites):
        d0, d1 = 0.25 * mob_hp(lvl), 0.24 * mob_hp(nxt)
        out.append(spell(sid, name, C, lvl, "nuke", d0 / 1.0, 2.5, 0, 60, "enemy", "magic",
                         [{"type": "damage", **scaled(d0, d1, lvl, nxt)}],
                         "Strike your foe with a lance of holy force. Weaker than a wizard's fire, but it serves.", stack_group="cleric_smite", rank=rank))

    stuns = [("halting_word", "Halting Word", 7, 2.0, 25), ("stilling_light", "Stilling Light", 24, 3.0, 45), ("hold_the_wicked", "Hold the Wicked", 43, 4.0, 62)]
    for rank, (sid, name, lvl, dur, cap) in enumerate(stuns, 1):
        out.append(spell(sid, name, C, lvl, "cc", 10 + lvl * 1.5, 1.0, 10, 40, "enemy", "magic",
                         [{"type": "damage", "base": 2 + lvl, "per_level": 0.2, "max": 3 + lvl * 2},
                          {"type": "stun", "duration": dur, "max_target_level": cap}],
                         f"A word of power that strikes and stuns your target for {dur:g} seconds. The stun fails on foes above level {cap}.",
                         stack_group="cleric_stun", rank=rank))

    rezzes = [("rite_of_return", "Rite of Return", 18, 50), ("breath_of_life", "Breath of Life", 36, 75), ("sanctified_return", "Sanctified Return", 53, 96)]
    for rank, (sid, name, lvl, pct) in enumerate(rezzes, 1):
        out.append(spell(sid, name, C, lvl, "utility", 20 + lvl * 3, 6.0, 0, 20, "corpse", "none",
                         [{"type": "resurrect", "value": pct}],
                         f"Call a fallen ally's spirit back to their corpse, restoring {pct}% of the experience their death cost them.",
                         stack_group="cleric_rez", rank=rank))

    out.append(spell("rebuke_the_dead", "Rebuke the Dead", C, 16, "cc", 35, 2.0, 12, 40, "enemy", "magic",
                     [{"type": "fear", "duration": 12, "max_target_level": 28}],
                     "Fill an undead foe with holy terror: it flees for 12 seconds. No effect above level 28.", requires_body="undead"))
    out.append(spell("purifying_touch", "Purifying Touch", C, 11, "utility", 25, 2.0, 0, 40, "friendly", "none",
                     [{"type": "dispel", "value": 2}], "Cleanse up to two harmful effects from your target."))
    return out


def price(level: int) -> int:
    return int(5 + 2 * level * level)


def main() -> None:
    spells = wizard() + cleric()
    spells.sort(key=lambda s: (s["class"], s["level"], s["id"]))
    lines = ["{", '  "_comment": ' + json.dumps(
        "Spells (generated by tools/gen_spells.py; hand-tune here or in the generator). Learned from scroll items, memorized "
        "into gems, and cast with mana. Effects drive the mechanics; see docs/DECISIONS.md for stacking and resist rules.") + ",",
             '  "spells": [']
    for i, s in enumerate(spells):
        lines.append("    " + json.dumps(s) + ("," if i < len(spells) - 1 else ""))
    lines += ["  ]", "}", ""]
    spells_text = "\n".join(lines)

    # Scrolls: replace every scroll item, keep everything else in file order. (Every input is read
    # and every output rendered before the first write, so a bad input changes nothing.)
    idoc = json.loads((CONTENT / "items.json").read_text(encoding="utf-8"))
    items = [it for it in idoc["items"] if it.get("kind") != "scroll"]
    scrolls = []
    for s in spells:
        scrolls.append({"id": "scroll_" + s["id"], "name": "Spell: " + s["name"], "kind": "scroll", "stack": 1,
                        "value": price(s["level"]), "rarity": "common", "spell": s["id"],
                        "description": f"A {s['class']} spell scroll (level {s['level']}). Scribe it into your spellbook to learn {s['name']}."})
    rows = items + scrolls
    lines = ["{", '  "_comment": ' + json.dumps(idoc.get("_comment", "")) + ",", '  "items": [']
    for i, r in enumerate(rows):
        lines.append("    " + json.dumps(r) + ("," if i < len(rows) - 1 else ""))
    lines += ["  ]", "}", ""]
    items_text = "\n".join(lines)

    by_class = lambda c: ["scroll_" + s["id"] for s in spells if s["class"] == c]
    npcs = [
        {"id": "sister_maelis", "name": "Sister Maelis", "role": "trainer", "model": "humanoid", "level": 55, "zone": ZONE,
         "x": 6.0, "z": 6.0, "yaw": 3.14, "classes": ["cleric"], "sells": by_class("cleric"),
         "greeting": "Light keep you, child. Every prayer I know is written in these scrolls; choose what your faith can bear."},
        {"id": "archmage_torvan", "name": "Archmage Torvan Quill", "role": "trainer", "model": "humanoid", "level": 58, "zone": ZONE,
         "x": -6.0, "z": 6.0, "yaw": 3.14, "classes": ["wizard"], "sells": by_class("wizard"),
         "greeting": "Ah, an apprentice of the weave. Fire, frost, or pure force? Mind the cost, it is always paid."},
    ]
    lines = ["{", '  "_comment": ' + json.dumps("Non-combat NPCs (generated by tools/gen_spells.py). Trainers sell spell scrolls to the classes they teach.") + ",",
             '  "npcs": [']
    for i, n in enumerate(npcs):
        lines.append("    " + json.dumps(n) + ("," if i < len(npcs) - 1 else ""))
    lines += ["  ]", "}", ""]
    npcs_text = "\n".join(lines)
    for name, text in (("spells.json", spells_text), ("items.json", items_text), ("npcs.json", npcs_text)):
        json.loads(text)  # sanity: every output parses
    for name, text in (("spells.json", spells_text), ("items.json", items_text), ("npcs.json", npcs_text)):
        (CONTENT / name).write_text(text, encoding="utf-8", newline="\n")
    counts = {c: sum(1 for s in spells if s["class"] == c) for c in ("cleric", "wizard")}
    print(f"wrote {len(spells)} spells {counts}, {len(scrolls)} scrolls, {len(npcs)} trainers")


if __name__ == "__main__":
    main()
