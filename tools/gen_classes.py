#!/usr/bin/env python3
"""Generates every class's abilities and spells (Phase 4) from tiered templates.

Writes, all committed and hand-editable:
  content/abilities.json  class abilities (endurance skills, disciplines, passives)
  content/spells.json     every spell and Bard song (Cleric and Wizard lines come from gen_spells.py)
  content/pets.json       pet templates (summoned servants, undead, spirits, animated blades)
  content/items.json      one scroll per spell, plus the summoned items (other items are kept)
  content/npcs.json       one trainer per spell-using class, and a practice lockbox

Numbers come from the same curves the server uses (rules.json mob_scaling, classes.json), so a rank
learned at level L is sized against an even-level mob at L and grows until the next rank. Signature
abilities are hand-tuned below. Afterwards the generator checks the class shape rules (rules.json
"content": at least min_class_abilities per class, something new at least every max_level_gap levels).

Usage: python tools/gen_classes.py [--check]   (--check: fail if the committed files are stale; CI runs it)
"""
from __future__ import annotations

import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parent))
from npc_order import ordered, render as render_npcs  # noqa: E402
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_spells as base  # noqa: E402  (Cleric and Wizard lines, shared curves)

ROOT = base.ROOT
CONTENT = base.CONTENT
MAX_LEVEL = base.MAX_LEVEL
ZONE = base.ZONE
mob_hp = base.mob_hp
class_hp = base.class_hp
scaled = base.scaled
ladder = base.ladder
spell = base.spell


# ------------------------------------------------------------------------------------------- helpers

def ability(aid, name, cls, lvl, typ, effects, desc, *, cost=0, cd=0.0, rng=0.0, target="enemy", gcd=True, skill=None,
            resource="endurance", cast=0.0, **extra):
    row = {"id": aid, "name": name, "class": cls, "level": lvl, "type": typ, "resource": resource, "cost": int(round(cost)),
           "cast_time": cast, "cooldown": cd, "gcd": gcd, "range": rng, "target": target, "resist": "none"}
    if skill:
        row["skill"] = skill
    row.update(extra)
    row["effects"] = effects
    row["description"] = desc
    return row


def passive(aid, name, cls, lvl, skill, desc):
    return {"id": aid, "name": name, "class": cls, "level": lvl, "type": "passive", "skill": skill, "description": desc}


def song(sid, name, cls, lvl, typ, rng, target, resist, effects, desc, **extra):
    row = spell(sid, name, cls, lvl, typ, 0, 0.0, 0, rng, target, resist, effects, desc, **extra)
    row["resource"] = "none"
    row["cast_time"] = 0
    row["song"] = True
    row["skill"] = "singing"
    return row


def dmg(frac0, frac1, lvl, nxt):
    """A damage effect sized as a fraction of an even-level mob's health."""
    return {"type": "damage", **scaled(frac0 * mob_hp(lvl), frac1 * mob_hp(nxt), lvl, nxt)}


def heal_amt(frac0, frac1, lvl, nxt, who="warrior"):
    return scaled(frac0 * class_hp(who, lvl), frac1 * class_hp(who, nxt), lvl, nxt)


def tiers(names_levels):
    """[(rank, level, next level, id, name)] for a tiered line given [(id, name, level), ...]."""
    out = []
    for (rank, lvl, nxt), (sid, name, _) in zip(ladder([x[2] for x in names_levels]), names_levels):
        out.append((rank, lvl, nxt, sid, name))
    return out


def ab_nuke(cls, line, frac, *, cost_per_level=0.35, cd=8, rng=4, skill=None, group=None, desc="", position=None, extra_effects=None):
    out = []
    for rank, lvl, nxt, sid, name in tiers(line):
        effects = [dmg(frac, frac * 0.95, lvl, nxt)] + (extra_effects(lvl) if extra_effects else [])
        kw = {"stack_group": group, "rank": rank} if group else {}
        if position:
            kw["position"] = position
        out.append(ability(sid, name, cls, lvl, "nuke", effects, desc.format(rank=rank), cost=4 + cost_per_level * lvl, cd=cd, rng=rng,
                           skill=skill, **kw))
    return out


def sp_nuke(cls, line, frac, resist, eff, *, cast=2.0, rng=60, group, desc, extra=None, cd=0, **kw):
    out = []
    for rank, lvl, nxt, sid, name in tiers(line):
        d0 = frac * mob_hp(lvl)
        effects = [dmg(frac, frac * 0.95, lvl, nxt)] + (extra(rank, lvl) if extra else [])
        out.append(spell(sid, name, cls, lvl, "nuke", d0 / (eff + 0.01 * lvl), cast + 0.15 * (rank - 1), cd, rng, "enemy", resist, effects,
                         desc.format(rank=rank), stack_group=group, rank=rank, **kw))
    return out


def sp_dot(cls, line, total_frac, resist, eff, *, dur=18, tick=3, group, desc, typ="dot", kind="dot", value=None, cast=2.0):
    out = []
    ticks = dur / tick
    for rank, lvl, nxt, sid, name in tiers(line):
        t0, t1 = total_frac * mob_hp(lvl), total_frac * 0.95 * mob_hp(nxt)
        e = {"type": kind, **scaled(t0 / ticks, t1 / ticks, lvl, nxt), "duration": dur, "tick": tick}
        if value is not None:
            e["value"] = value
        out.append(spell(sid, name, cls, lvl, typ, t0 / (eff + 0.01 * lvl), cast, 0, 60, "enemy", resist, [e], desc.format(rank=rank, dur=dur),
                         stack_group=group, rank=rank))
    return out


def sp_heal(cls, line, frac, eff, *, cast=2.5, rng=60, group, desc, target="friendly"):
    out = []
    for rank, lvl, nxt, sid, name in tiers(line):
        h = heal_amt(frac, frac * 0.93, lvl, nxt)
        out.append(spell(sid, name, cls, lvl, "heal", (frac * class_hp("warrior", lvl)) / (eff + 0.02 * lvl), cast, 0, rng, target, "none",
                         [{"type": "heal", **h}], desc.format(rank=rank), stack_group=group, rank=rank))
    return out


def sp_buff(cls, rows, *, group, typ="buff", target="friendly", dur=1800, cast=3.0, rng=60, cost_base=10, cost_per=2.0, desc):
    """rows: [(id, name, level, {stat: value})]."""
    out = []
    for rank, (sid, name, lvl, stats) in enumerate(rows, 1):
        effects = [{"type": "buff", "stat": st, "value": v, "duration": dur} for st, v in stats.items()]
        text = ", ".join(f"{'+' if v > 0 else ''}{v:g}{'%' if st.endswith('_pct') else ''} {st.replace('_pct', '').replace('_', ' ')}" for st, v in stats.items())
        out.append(spell(sid, name, cls, lvl, typ, cost_base + cost_per * lvl, cast, 0, rng, target, "none", effects,
                         desc.format(stats=text, minutes=dur // 60, seconds=dur), stack_group=group, rank=rank))
    return out


def sp_pet(cls, rows, pet_id, *, group, desc, cast=6.0, typ="pet"):
    """rows: [(id, name, level, level cap)] -> summons pet_id at min(caster level, cap)."""
    out = []
    for rank, (sid, name, lvl, cap) in enumerate(rows, 1):
        out.append(spell(sid, name, cls, lvl, typ, 15 + lvl * 2.5, cast, 6, 0, "self", "none",
                         [{"type": "summon_pet", "pet": pet_id, "base": 0, "per_level": 1, "max": cap}],
                         desc.format(cap=cap), stack_group=group, rank=rank))
    return out


# ------------------------------------------------------------------------------------------- warrior

def warrior() -> tuple[list[dict], list[dict]]:
    W = "warrior"
    a: list[dict] = []
    # The Phase 2 kit, kept exactly.
    a.append(ability("iron_challenge", "Iron Challenge", W, 1, "utility", [{"type": "taunt"}],
                     "Bellow a challenge that makes your target attack you instead of your allies.", cd=6, rng=20, skill="taunt"))
    a.append(ability("rising_kick", "Rising Kick", W, 1, "nuke", [{"type": "damage", "base": 3, "per_level": 1.2}],
                     "A snapping kick that deals light damage.", cost=4, cd=6, rng=4, skill="kick"))
    a.append(ability("pommel_crack", "Pommel Crack", W, 6, "cc", [{"type": "damage", "base": 2, "per_level": 0.6}, {"type": "stun", "duration": 2.0}],
                     "Crack your foe with the pommel of your weapon, stunning it briefly. Fails on foes well above your level.", cost=8, cd=12, rng=4, skill="bash"))
    a.append(passive("sidestep", "Dodge", W, 6, "dodge", "Passive: a chance to dodge melee attacks. Improves with use."))
    a.append(passive("turning_blade", "Parry", W, 10, "parry", "Passive: a chance to parry melee attacks. Improves with use."))
    a.append(ability("rallying_cry", "Rallying Cry", W, 12, "heal", [{"type": "heal_pct", "value": 15}, {"type": "restore_endurance_pct", "value": 25}],
                     "Steel yourself with a war cry, recovering some health and endurance.", cd=180, target="self"))
    a.append(passive("twin_strikes", "Double Attack", W, 15, "double_attack", "Passive: a chance to swing twice each round. Improves with use."))
    a.append(ability("cleaving_blow", "Cleaving Blow", W, 16, "nuke", [{"type": "damage", "base": 12, "per_level": 2.0}, {"type": "threat", "base": 20, "per_level": 1.0}],
                     "A heavy overhead blow that deals solid damage and draws attention.", cost=15, cd=10, rng=4))
    a.append(ability("bulwark_stance", "Bulwark Stance", W, 20, "discipline", [{"type": "buff", "stat": "damage_taken_pct", "value": -40, "duration": 15}],
                     "Defensive discipline: take 40% less melee damage for 15 seconds.", cost=20, cd=300, gcd=False, target="self"))
    a.append(ability("provoking_roar", "Provoking Roar", W, 24, "utility", [{"type": "area_threat", "radius": 15, "base": 40, "per_level": 4.0}],
                     "Roar at every foe within 15 meters that is already fighting, drawing their anger to you.", cost=15, cd=20, target="self"))
    a.append(passive("counterstroke", "Riposte", W, 25, "riposte", "Passive: a chance to block a melee attack and strike back at once. Improves with use."))
    a.append(ability("battle_fury", "Battle Fury", W, 30, "discipline",
                     [{"type": "buff", "stat": "melee_damage_pct", "value": 35, "duration": 20}, {"type": "buff", "stat": "crit_chance_pct", "value": 10, "duration": 20}],
                     "Offensive discipline: deal 35% more melee damage with a higher critical chance for 20 seconds.", cost=25, cd=300, gcd=False, target="self"))
    a.append(ability("stalwart_resolve", "Stalwart Resolve", W, 40, "heal", [{"type": "heal_pct", "value": 30}],
                     "Refuse to fall: instantly recover 30% of your health.", cd=900, gcd=False, target="self"))
    a.append(ability("earthsplitter", "Earthsplitter", W, 46, "nuke", [{"type": "damage", "base": 40, "per_level": 3.5}, {"type": "threat", "base": 60, "per_level": 2.0}],
                     "Drive your weapon down with ground-shaking force. A stronger rank of Cleaving Blow.", cost=30, cd=15, rng=4))
    a.append(ability("unbroken_wall", "Unbroken Wall", W, 52, "discipline", [{"type": "buff", "stat": "damage_taken_pct", "value": -65, "duration": 10}],
                     "Defensive discipline: take 65% less melee damage for 10 seconds. A stronger rank of Bulwark Stance.", cost=30, cd=1200, gcd=False, target="self"))
    a.append(ability("warlords_challenge", "Warlord's Challenge", W, 56, "utility", [{"type": "taunt"}, {"type": "threat", "base": 100, "per_level": 4.0}],
                     "A stronger rank of Iron Challenge that also adds a large burst of extra anger.", cost=10, cd=6, rng=25, skill="taunt"))
    # Phase 4: the rest of the Warrior's list.
    a.append(ability("shoulder_check", "Shoulder Check", W, 3, "nuke", [{"type": "damage", "base": 4, "per_level": 0.9}, {"type": "threat", "base": 10, "per_level": 0.8}],
                     "Slam into your foe with your shoulder: light damage and a little extra anger.", cost=5, cd=8, rng=4))
    a.append(ability("hurled_stone", "Hurled Stone", W, 4, "utility", [{"type": "damage", "base": 2, "per_level": 0.4}, {"type": "threat", "base": 15, "per_level": 0.5}],
                     "Throw a stone at a distant foe to pull it to you.", cost=2, cd=10, rng=30, skill="throwing"))
    a.append(ability("second_wind", "Second Wind", W, 8, "heal", [{"type": "restore_endurance_pct", "value": 40}],
                     "Catch your breath and recover 40% of your endurance.", cd=240, gcd=False, target="self"))
    a.append(passive("off_hand_drill", "Dual Wield", W, 13, "dual_wield", "Passive: swing a weapon in your off hand too. Improves with use."))
    a.append(ability("hamstring_cut", "Hamstring Cut", W, 18, "debuff", [{"type": "damage", "base": 4, "per_level": 0.8}, {"type": "snare", "value": 40, "duration": 12}],
                     "Slash at your foe's legs: it moves 40% slower for 12 seconds.", cost=12, cd=15, rng=4))
    a.append(passive("heavy_hands", "Enhanced Damage", W, 21, "enhanced_damage", "Passive: your melee blows land harder. Improves with use."))
    a.append(ability("goading_strike", "Goading Strike", W, 27, "nuke", [{"type": "damage", "base": 8, "per_level": 1.6}, {"type": "threat", "base": 40, "per_level": 2.0}],
                     "A mocking strike that deals damage and draws a lot of anger.", cost=14, cd=8, rng=4))
    a.append(ability("iron_hate", "Iron Hate", W, 33, "discipline", [{"type": "buff", "stat": "threat_pct", "value": 100, "duration": 30}],
                     "Hate discipline: everything you do angers your foes twice as much for 30 seconds.", cost=20, cd=180, gcd=False, target="self"))
    a.append(ability("sundering_blow", "Sundering Blow", W, 36, "debuff", [{"type": "damage", "base": 10, "per_level": 1.4}, {"type": "debuff", "stat": "ac", "value": -30, "duration": 30}],
                     "Hew at your foe's armor, lowering its armor by 30 for 30 seconds.", cost=18, cd=20, rng=4))
    a.append(ability("skullringer", "Skullringer", W, 38, "cc", [{"type": "damage", "base": 8, "per_level": 1.0}, {"type": "stun", "duration": 3.0}],
                     "A ringing blow to the head that stuns for 3 seconds. A stronger rank of Pommel Crack.", cost=16, cd=12, rng=4, skill="bash"))
    a.append(ability("titans_challenge", "Titan's Challenge", W, 43, "utility", [{"type": "area_threat", "radius": 20, "base": 120, "per_level": 6.0}],
                     "Roar at every engaged foe within 20 meters. A stronger rank of Provoking Roar.", cost=25, cd=20, target="self"))
    a.append(ability("warcry_of_ages", "Warcry of Ages", W, 49, "heal", [{"type": "heal_pct", "value": 25}, {"type": "restore_endurance_pct", "value": 40}],
                     "A stronger rank of Rallying Cry.", cd=180, target="self"))
    a.append(ability("berserkers_fury", "Berserker's Fury", W, 54, "discipline",
                     [{"type": "buff", "stat": "melee_damage_pct", "value": 60, "duration": 20}, {"type": "buff", "stat": "attack_speed_pct", "value": 25, "duration": 20}],
                     "Offensive discipline: 60% more melee damage and faster swings for 20 seconds.", cost=35, cd=600, gcd=False, target="self"))
    a.append(ability("worldbreaker", "Worldbreaker", W, 59, "nuke", [{"type": "damage", "base": 80, "per_level": 5.0}, {"type": "threat", "base": 150, "per_level": 3.0}],
                     "A blow that could crack mountains. The strongest rank of Cleaving Blow.", cost=40, cd=15, rng=4))
    return a, []


# ------------------------------------------------------------------------------------------- paladin

def paladin() -> tuple[list[dict], list[dict]]:
    P = "paladin"
    a = [
        ability("vow_of_challenge", "Vow of Challenge", P, 1, "utility", [{"type": "taunt"}], "Call your target to face you instead of your allies.", cd=6, rng=20, skill="taunt"),
        ability("hallowed_strike", "Hallowed Strike", P, 1, "nuke", [{"type": "damage", "base": 3, "per_level": 1.1}], "A blessed weapon strike.", cost=4, cd=6, rng=4),
        ability("shield_rebuke", "Shield Rebuke", P, 4, "cc", [{"type": "damage", "base": 2, "per_level": 0.5}, {"type": "stun", "duration": 2.0}],
                "Bash your foe with your shield, stunning it briefly.", cost=8, cd=12, rng=4, skill="bash"),
        ability("hand_of_mercy", "Hand of Mercy", P, 6, "heal", [{"type": "heal_pct", "value": 100}],
                "Lay your hands on yourself and be made whole. Once an hour.", cd=3600, gcd=False, target="self"),
        passive("pal_dodge", "Dodge", P, 8, "dodge", "Passive: a chance to dodge melee attacks."),
        passive("pal_parry", "Parry", P, 17, "parry", "Passive: a chance to parry melee attacks."),
        passive("pal_double", "Double Attack", P, 20, "double_attack", "Passive: a chance to swing twice each round."),
        ability("sacred_bulwark", "Sacred Bulwark", P, 25, "discipline", [{"type": "buff", "stat": "damage_taken_pct", "value": -35, "duration": 15}],
                "Defensive discipline: take 35% less melee damage for 15 seconds.", cost=20, cd=300, gcd=False, target="self"),
        passive("pal_riposte", "Riposte", P, 30, "riposte", "Passive: a chance to riposte melee attacks."),
        ability("zealots_oath", "Zealot's Oath", P, 39, "discipline", [{"type": "buff", "stat": "threat_pct", "value": 80, "duration": 30}],
                "Hate discipline: your deeds anger foes 80% more for 30 seconds.", cost=20, cd=180, gcd=False, target="self"),
        ability("radiant_vow", "Radiant Vow", P, 49, "utility", [{"type": "taunt"}, {"type": "threat", "base": 80, "per_level": 3.0}],
                "A stronger rank of Vow of Challenge that adds a burst of anger.", cost=10, cd=6, rng=25, skill="taunt"),
        ability("sunbreaker", "Sunbreaker", P, 51, "nuke", [{"type": "damage", "base": 40, "per_level": 3.0}, {"type": "threat", "base": 60, "per_level": 2.0}],
                "A radiant blow that burns and enrages.", cost=30, cd=15, rng=4),
    ]
    s: list[dict] = []
    s += sp_heal(P, [("lay_light", "Lay Light", 9), ("devout_mending", "Devout Mending", 23), ("oath_of_renewal", "Oath of Renewal", 41)],
                 0.3, 1.6, group="paladin_heal", desc="Mend your target's wounds. Rank {rank} of the paladin heal.")
    s += sp_nuke(P, [("purge_the_dead", "Purge the Dead", 11), ("sanctified_flame", "Sanctified Flame", 29), ("dawns_judgment", "Dawn's Judgment", 47)],
                 0.4, "magic", 1.8, group="paladin_undead", desc="Holy fire that only harms the undead. Rank {rank}.", requires_body="undead", resist_adjust=-20)
    s += [spell("stunning_vow", "Stunning Vow", P, 14, "cc", 25, 1.0, 12, 40, "enemy", "magic",
                [{"type": "stun", "duration": 2.5, "max_target_level": 30}], "A word of faith that stuns your target. No effect above level 30.",
                stack_group="paladin_stun", rank=1),
          spell("holy_halt", "Holy Halt", P, 38, "cc", 55, 1.0, 12, 40, "enemy", "magic",
                [{"type": "stun", "duration": 3.5, "max_target_level": 55}], "A stronger stunning word. No effect above level 55.",
                stack_group="paladin_stun", rank=2)]
    s += sp_buff(P, [("vow_of_steel", "Vow of Steel", 12, {"ac": 6, "max_hp": 30}), ("oath_of_iron", "Oath of Iron", 26, {"ac": 12, "max_hp": 80}),
                     ("oath_of_the_bastion", "Oath of the Bastion", 44, {"ac": 20, "max_hp": 170})],
                 group="paladin_vow", desc="Bless your target: {stats} for {minutes} minutes.")
    s += sp_buff(P, [("righteous_zeal", "Righteous Zeal", 16, {"melee_damage_pct": 10, "mana_regen": 1}),
                     ("fervent_zeal", "Fervent Zeal", 35, {"melee_damage_pct": 18, "mana_regen": 2}),
                     ("holy_fervor", "Holy Fervor", 56, {"melee_damage_pct": 25, "mana_regen": 3})],
                 group="paladin_zeal", target="self", dur=180, cast=0.0, desc="Fill yourself with zeal: {stats} for {minutes} minutes.")
    s += [spell("faiths_return", "Faith's Return", P, 32, "utility", 90, 6.0, 0, 20, "corpse", "none", [{"type": "resurrect", "value": 20}],
                "Call a fallen ally back to their corpse, restoring 20% of the experience they lost."),
          spell("cleansing_vow", "Cleansing Vow", P, 21, "utility", 30, 2.0, 0, 40, "friendly", "none", [{"type": "dispel", "value": 1}],
                "Cleanse one harmful effect from your target."),
          spell("ward_of_valor", "Ward of Valor", P, 53, "buff", 120, 3.0, 0, 60, "friendly", "none",
                [{"type": "buff", "stat": "resist_all", "value": 25, "duration": 1800}], "Shield your target against magic: +25 to all resists for 30 minutes."),
          spell("reverent_call", "Reverent Call", P, 59, "utility", 40, 1.0, 10, 40, "enemy", "magic", [{"type": "threat", "base": 200, "per_level": 4}],
                "Call down your faith upon a foe, drawing its full attention.")]
    return a, s


# ------------------------------------------------------------------------------------------- shadow knight

def shadow_knight() -> tuple[list[dict], list[dict]]:
    K = "shadow_knight"
    a = [
        ability("dread_challenge", "Dread Challenge", K, 1, "utility", [{"type": "taunt"}], "A chilling challenge that draws your target to you.", cd=6, rng=20, skill="taunt"),
        ability("gloomstrike", "Gloomstrike", K, 1, "nuke", [{"type": "damage", "base": 3, "per_level": 1.1}], "A strike wreathed in shadow.", cost=4, cd=6, rng=4),
        ability("cruel_bash", "Cruel Bash", K, 4, "cc", [{"type": "damage", "base": 2, "per_level": 0.5}, {"type": "stun", "duration": 2.0}],
                "Bash your foe, stunning it briefly.", cost=8, cd=12, rng=4, skill="bash"),
        ability("grave_touch", "Grave Touch", K, 6, "nuke", [{"type": "damage", "base": 30, "per_level": 9.0}],
                "Touch your foe with death itself, dealing enormous damage. Once an hour.", cost=0, cd=3600, gcd=False, rng=5),
        passive("sk_dodge", "Dodge", K, 8, "dodge", "Passive: a chance to dodge melee attacks."),
        passive("sk_parry", "Parry", K, 17, "parry", "Passive: a chance to parry melee attacks."),
        passive("sk_double", "Double Attack", K, 20, "double_attack", "Passive: a chance to swing twice each round."),
        ability("dark_aegis", "Dark Aegis", K, 25, "discipline", [{"type": "buff", "stat": "damage_taken_pct", "value": -35, "duration": 15}],
                "Defensive discipline: take 35% less melee damage for 15 seconds.", cost=20, cd=300, gcd=False, target="self"),
        passive("sk_riposte", "Riposte", K, 30, "riposte", "Passive: a chance to riposte melee attacks."),
        ability("bloodied_challenge", "Bloodied Challenge", K, 45, "utility", [{"type": "taunt"}, {"type": "threat", "base": 80, "per_level": 3.0}],
                "A stronger rank of Dread Challenge that adds a burst of anger.", cost=10, cd=6, rng=25, skill="taunt"),
        ability("spite_unbound", "Spite Unbound", K, 47, "discipline", [{"type": "buff", "stat": "threat_pct", "value": 80, "duration": 30}],
                "Hate discipline: your deeds anger foes 80% more for 30 seconds.", cost=20, cd=180, gcd=False, target="self"),
    ]
    s: list[dict] = []
    for rank, lvl, nxt, sid, name in tiers([("siphon_life", "Siphon Life", 9), ("vampiric_touch", "Vampiric Touch", 22), ("soul_leech", "Soul Leech", 37),
                                             ("blood_harvest", "Blood Harvest", 52)]):
        h0, h1 = 0.28 * mob_hp(lvl), 0.27 * mob_hp(nxt)
        s.append(spell(sid, name, K, lvl, "nuke", h0 / (1.5 + 0.01 * lvl), 2.0, 6, 40, "enemy", "disease",
                       [{"type": "lifetap", **scaled(h0, h1, lvl, nxt), "value": 100}, {"type": "threat", "base": 10, "per_level": 1.0}],
                       f"Tear the life from your target and take it as your own. Rank {rank} of the lifetap line.", stack_group="sk_tap", rank=rank))
    s += [spell("dread_gaze", "Dread Gaze", K, 11, "cc", 25, 1.5, 10, 40, "enemy", "magic", [{"type": "fear", "duration": 10, "max_target_level": 26}],
                "Your gaze fills your target with terror: it flees for 10 seconds. No effect above level 26.", stack_group="sk_fear", rank=1),
          spell("visage_of_doom", "Visage of Doom", K, 34, "cc", 60, 1.5, 10, 40, "enemy", "magic", [{"type": "fear", "duration": 14, "max_target_level": 52}],
                "A stronger rank of Dread Gaze. No effect above level 52.", stack_group="sk_fear", rank=2)]
    s += sp_pet(K, [("raise_squire", "Raise Bone Squire", 15, 14), ("raise_dread_squire", "Raise Dread Squire", 35, 30),
                    ("raise_grave_champion", "Raise Grave Champion", 55, 48)],
                "bone_squire", group="sk_pet", desc="Raise a skeletal squire to fight at your side (up to level {cap}).")
    s += sp_dot(K, [("rotting_grip", "Rotting Grip", 13), ("plague_grasp", "Plague Grasp", 31), ("blight_embrace", "Blight Embrace", 49)],
                0.45, "disease", 1.4, group="sk_dot", desc="Rot spreads through your target for {dur} seconds. Rank {rank}.")
    s += [spell("grasping_dark", "Grasping Dark", K, 18, "debuff", 30, 1.5, 0, 40, "enemy", "disease", [{"type": "snare", "value": 40, "duration": 30}],
                "Shadows drag at your target's feet: it moves 40% slower for 30 seconds."),
          spell("deathly_guise", "Deathly Guise", K, 27, "utility", 30, 0.0, 30, 0, "self", "none", [{"type": "feign_death", "value": 75}],
                "Fall as if dead (75% chance to fool your foes)."),
          spell("terror_of_night", "Terror of Night", K, 28, "utility", 35, 1.0, 6, 40, "enemy", "magic", [{"type": "threat", "base": 60, "per_level": 3}],
                "Strike terror into a foe, drawing its full anger."),
          spell("shroud_of_spite", "Shroud of Spite", K, 40, "buff", 80, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "damage_shield", "value": 12, "duration": 600}, {"type": "buff", "stat": "melee_damage_pct", "value": 10, "duration": 600}],
                "Cloak yourself in spite: attackers take 12 damage per hit and your blows strike 10% harder for 10 minutes."),
          spell("sap_strength", "Sap Strength", K, 43, "debuff", 70, 2.0, 0, 40, "enemy", "disease", [{"type": "debuff", "stat": "melee_damage_pct", "value": -15, "duration": 60}],
                "Weaken your target: it hits 15% softer for one minute."),
          spell("night_terror_wave", "Night Terror Wave", K, 58, "utility", 70, 1.5, 15, 0, "area", "magic", [{"type": "threat", "base": 150, "per_level": 4}],
                "Anger every foe within 15 meters of you.", radius=15)]
    return a, s


# ------------------------------------------------------------------------------------------- ranger

def ranger() -> tuple[list[dict], list[dict]]:
    R = "ranger"
    a = [
        ability("quick_shot", "Quick Shot", R, 1, "nuke", [{"type": "damage", "base": 4, "per_level": 1.2}], "Loose a quick arrow at your target (needs a bow).",
                cost=4, cd=6, rng=30, skill="archery", requires_weapon="archery", stack_group="ranger_shot", rank=1),
        ability("trail_sense", "Trail Sense", R, 1, "utility", [{"type": "track", "radius": 150, "value": 3}],
                "Read the land for the creatures around you. The range grows with your tracking.", cd=5, gcd=False, target="self", skill="tracking"),
        ability("hunters_mark", "Hunter's Mark", R, 3, "debuff", [{"type": "debuff", "stat": "ac", "value": -6, "duration": 60}],
                "Mark your quarry: its armor drops by 6 for a minute.", cost=4, cd=20, rng=30),
        ability("ranger_kick", "Snap Kick", R, 5, "nuke", [{"type": "damage", "base": 3, "per_level": 1.0}], "A quick kick.", cost=4, cd=8, rng=4, skill="kick"),
        passive("rng_dodge", "Dodge", R, 8, "dodge", "Passive: a chance to dodge melee attacks."),
        passive("rng_dual", "Dual Wield", R, 17, "dual_wield", "Passive: swing a weapon in each hand."),
        passive("rng_parry", "Parry", R, 18, "parry", "Passive: a chance to parry melee attacks."),
        passive("rng_double", "Double Attack", R, 20, "double_attack", "Passive: a chance to swing twice each round."),
        ability("piercing_shot", "Piercing Shot", R, 15, "nuke", [{"type": "damage", "base": 10, "per_level": 2.2}], "A powerful aimed arrow.",
                cost=12, cd=8, rng=35, skill="archery", requires_weapon="archery", stack_group="ranger_shot", rank=2),
        ability("hawkeye_volley", "Hawkeye Volley", R, 33, "nuke", [{"type": "damage", "base": 20, "per_level": 3.0}], "A volley of aimed arrows.",
                cost=20, cd=8, rng=40, skill="archery", requires_weapon="archery", stack_group="ranger_shot", rank=3),
        ability("heartseeker", "Heartseeker", R, 50, "nuke", [{"type": "damage", "base": 40, "per_level": 4.0}], "An arrow that finds the heart.",
                cost=28, cd=8, rng=45, skill="archery", requires_weapon="archery", stack_group="ranger_shot", rank=4),
        passive("rng_riposte", "Riposte", R, 35, "riposte", "Passive: a chance to riposte melee attacks."),
        ability("predators_focus", "Predator's Focus", R, 45, "discipline",
                [{"type": "buff", "stat": "crit_chance_pct", "value": 15, "duration": 30}, {"type": "buff", "stat": "melee_damage_pct", "value": 20, "duration": 30}],
                "Offensive discipline: sharper strikes for 30 seconds.", cost=25, cd=300, gcd=False, target="self"),
    ]
    s: list[dict] = []
    s += [spell("grasping_vines", "Grasping Vines", R, 9, "debuff", 20, 1.5, 0, 40, "enemy", "magic", [{"type": "snare", "value": 40, "duration": 30}],
                "Vines catch your target's legs: it moves 40% slower for 30 seconds.", stack_group="ranger_snare", rank=1),
          spell("ensnaring_thorns", "Ensnaring Thorns", R, 30, "debuff", 45, 1.5, 0, 40, "enemy", "magic", [{"type": "snare", "value": 55, "duration": 45}],
                "A stronger snare: 55% slower for 45 seconds.", stack_group="ranger_snare", rank=2)]
    s += sp_heal(R, [("woodland_salve", "Woodland Salve", 10), ("forest_balm", "Forest Balm", 28), ("verdant_mending", "Verdant Mending", 48)],
                 0.22, 1.5, group="ranger_heal", desc="A light healing salve. Rank {rank}.")
    s += sp_buff(R, [("barkhide", "Barkhide", 12, {"ac": 5}), ("oakhide", "Oakhide", 32, {"ac": 12}), ("ironwood_hide", "Ironwood Hide", 52, {"ac": 20})],
                 group="ranger_skin", desc="Harden your target's skin: {stats} for {minutes} minutes.")
    s += sp_dot(R, [("flamebrand", "Flamebrand", 14), ("wildfire_brand", "Wildfire Brand", 36), ("sunfire_brand", "Sunfire Brand", 54)],
                0.4, "fire", 1.3, group="ranger_dot", desc="Brand your target with fire that burns for {dur} seconds. Rank {rank}.")
    s += [spell("binding_roots", "Binding Roots", R, 21, "cc", 30, 1.5, 0, 40, "enemy", "magic", [{"type": "root", "duration": 20, "max_target_level": 40}],
                "Roots hold your target in place for 20 seconds."),
          spell("wind_stride", "Wind Stride", R, 24, "buff", 30, 2.0, 0, 0, "self", "none", [{"type": "buff", "stat": "move_speed_pct", "value": 30, "duration": 1200}],
                "Run with the wind: 30% faster for 20 minutes."),
          spell("hunters_eye", "Hunter's Eye", R, 26, "buff", 40, 2.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "crit_chance_pct", "value": 6, "duration": 1800}, {"type": "buff", "stat": "dex", "value": 15, "duration": 1800}],
                "Sharpen your eye: +6% critical chance and +15 dexterity for 30 minutes."),
          spell("thorncoat", "Thorncoat", R, 38, "buff", 60, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "damage_shield", "value": 8, "duration": 600}],
                "Thorns sprout on your target: melee attackers take 8 damage per hit for 10 minutes."),
          spell("frost_arrow", "Frost Arrow", R, 41, "nuke", 70, 1.5, 6, 50, "enemy", "cold", [dmg(0.3, 0.28, 41, 60)],
                "A conjured arrow of ice."),
          spell("cloak_of_leaves", "Cloak of Leaves", R, 43, "buff", 40, 3.0, 0, 0, "self", "none", [{"type": "buff", "stat": "invisible", "value": 1, "duration": 600}],
                "Fade among the leaves: creatures cannot see you for 10 minutes, until you strike."),
          spell("call_of_the_wild", "Call of the Wild", R, 57, "buff", 90, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "attack_speed_pct", "value": 20, "duration": 600}, {"type": "buff", "stat": "str", "value": 20, "duration": 600}],
                "The wild quickens your blood: 20% faster attacks and +20 strength for 10 minutes.")]
    return a, s


# ------------------------------------------------------------------------------------------- bard

def bard() -> tuple[list[dict], list[dict]]:
    B = "bard"
    a = [
        ability("bard_track", "Traveler's Ear", B, 12, "utility", [{"type": "track", "radius": 100, "value": 2}],
                "Listen for the creatures around you.", cd=5, gcd=False, target="self", skill="tracking"),
        passive("bard_dodge", "Dodge", B, 10, "dodge", "Passive: a chance to dodge melee attacks."),
        passive("bard_dual", "Dual Wield", B, 17, "dual_wield", "Passive: swing a weapon in each hand."),
        passive("bard_parry", "Parry", B, 25, "parry", "Passive: a chance to parry melee attacks."),
        ability("bard_lockpick", "Tumbler's Touch", B, 30, "utility", [{"type": "pick_lock"}], "Coax a lock open.", cd=5, gcd=False, rng=5, skill="pick_lock"),
        ability("bard_kick", "Dancer's Kick", B, 3, "nuke", [{"type": "damage", "base": 3, "per_level": 0.9}], "A nimble kick.", cost=4, cd=8, rng=4, skill="kick"),
    ]
    D = 18  # every song effect lasts three pulses: twisting three songs keeps them all up
    s: list[dict] = []
    for rank, (sid, name, lvl, v) in enumerate([("hymn_of_mending", "Hymn of Mending", 1, 2), ("hymn_of_the_hearth", "Hymn of the Hearth", 22, 6),
                                                 ("hymn_of_renewal", "Hymn of Renewal", 44, 12)], 1):
        s.append(song(sid, name, B, lvl, "buff", 0, "group", "none", [{"type": "buff", "stat": "hp_regen", "value": v, "duration": D}],
                      f"A warm melody: +{v} health regeneration per second for you and your companions.", stack_group="bard_regen", rank=rank))
    for rank, (sid, name, lvl, v) in enumerate([("anthem_of_swiftblades", "Anthem of Swiftblades", 4, 10), ("anthem_of_the_storm", "Anthem of the Storm", 26, 20),
                                                 ("anthem_of_heroes", "Anthem of Heroes", 50, 30)], 1):
        s.append(song(sid, name, B, lvl, "buff", 0, "group", "none",
                      [{"type": "buff", "stat": "attack_speed_pct", "value": v, "duration": D}, {"type": "buff", "stat": "melee_damage_pct", "value": v // 2, "duration": D}],
                      f"A martial anthem: {v}% faster attacks and {v // 2}% harder blows.", stack_group="bard_haste", rank=rank))
    for rank, (sid, name, lvl, frac) in enumerate([("chant_of_embers", "Chant of Embers", 1, 0.08), ("chant_of_wildfire", "Chant of Wildfire", 20, 0.08),
                                                    ("chant_of_the_inferno", "Chant of the Inferno", 40, 0.08)], 1):
        nxt = [20, 40, 60][rank - 1]
        s.append(song(sid, name, B, lvl, "dot", 40, "enemy", "fire",
                      [{"type": "dot", **scaled(frac * mob_hp(lvl), frac * 0.95 * mob_hp(nxt), lvl, nxt), "duration": D, "tick": 3}],
                      "Sing fire into your target: it burns every 3 seconds while the song lasts.", stack_group="bard_fire", rank=rank))
    s += [song("lullaby_of_stillness", "Lullaby of Stillness", B, 9, "cc", 30, "enemy", "magic", [{"type": "mez", "duration": 12, "max_target_level": 25}],
               "Lull your target into a trance. No effect above level 25.", stack_group="bard_mez", rank=1),
          song("lullaby_of_deep_sleep", "Lullaby of Deep Sleep", B, 35, "cc", 30, "enemy", "magic", [{"type": "mez", "duration": 15, "max_target_level": 50}],
               "A deeper trance. No effect above level 50.", stack_group="bard_mez", rank=2),
          song("ballad_of_the_road", "Ballad of the Road", B, 6, "buff", 0, "group", "none", [{"type": "buff", "stat": "move_speed_pct", "value": 30, "duration": D}],
               "A walking song: 30% faster movement.", stack_group="bard_run", rank=1),
          song("ballad_of_the_gale", "Ballad of the Gale", B, 38, "buff", 0, "group", "none", [{"type": "buff", "stat": "move_speed_pct", "value": 50, "duration": D}],
               "Run like the wind: 50% faster movement.", stack_group="bard_run", rank=2),
          song("melody_of_clarity", "Melody of Clarity", B, 14, "buff", 0, "group", "none", [{"type": "buff", "stat": "mana_regen", "value": 3, "duration": D}],
               "A clear tune: +3 mana regeneration per second for you and your companions.", stack_group="bard_mana", rank=1),
          song("melody_of_insight", "Melody of Insight", B, 46, "buff", 0, "group", "none", [{"type": "buff", "stat": "mana_regen", "value": 7, "duration": D}],
               "+7 mana regeneration per second.", stack_group="bard_mana", rank=2),
          song("dirge_of_weariness", "Dirge of Weariness", B, 16, "debuff", 30, "enemy", "magic", [{"type": "slow", "value": 20, "duration": D}],
               "A dragging dirge: your target attacks 20% slower.", stack_group="bard_slow", rank=1),
          song("dirge_of_ages", "Dirge of Ages", B, 42, "debuff", 30, "enemy", "magic", [{"type": "slow", "value": 35, "duration": D}],
               "Your target attacks 35% slower.", stack_group="bard_slow", rank=2),
          song("requiem_of_rust", "Requiem of Rust", B, 18, "debuff", 30, "enemy", "magic", [{"type": "debuff", "stat": "ac", "value": -20, "duration": D}],
               "Your target's armor weakens by 20."),
          song("chord_of_warding", "Chord of Warding", B, 24, "buff", 0, "group", "none", [{"type": "buff", "stat": "resist_all", "value": 20, "duration": D}],
               "+20 to every resist."),
          song("lament_of_frost", "Lament of Frost", B, 28, "nuke", 30, "enemy", "cold", [dmg(0.06, 0.06, 28, 60)],
               "Each pulse strikes your target with a shard of cold song."),
          song("elegy_of_thorns", "Elegy of Thorns", B, 31, "buff", 0, "group", "none", [{"type": "buff", "stat": "damage_shield", "value": 10, "duration": D}],
               "Melee attackers of you and your companions take 10 damage per hit."),
          song("canticle_of_cinders", "Canticle of Cinders", B, 33, "dot", 0, "area", "fire",
               [{"type": "dot", **scaled(0.05 * mob_hp(33), 0.05 * mob_hp(60), 33, 60), "duration": D, "tick": 3}],
               "Burn every foe within 10 meters of you.", radius=10),
          song("verse_of_valor", "Verse of Valor", B, 48, "buff", 0, "group", "none",
               [{"type": "buff", "stat": "str", "value": 20, "duration": D}, {"type": "buff", "stat": "ac", "value": 15, "duration": D}],
               "+20 strength and +15 armor."),
          song("dirge_of_dust", "Dirge of Dust", B, 55, "debuff", 30, "enemy", "magic", [{"type": "snare", "value": 50, "duration": D}],
               "A dragging song: your target moves 50% slower."),
          song("serenade_of_silence", "Serenade of Silence", B, 53, "utility", 30, "enemy", "magic", [{"type": "threat_drop", "value": 50}],
               "Soothe your target: it forgets half its anger at you."),
          song("aria_of_the_ancients", "Aria of the Ancients", B, 57, "buff", 0, "group", "none",
               [{"type": "buff", "stat": "hp_regen", "value": 15, "duration": D}, {"type": "buff", "stat": "mana_regen", "value": 8, "duration": D}],
               "An ancient aria: +15 health and +8 mana regeneration per second.", stack_group="bard_regen", rank=4)]
    return a, s


# ------------------------------------------------------------------------------------------- rogue

def rogue() -> tuple[list[dict], list[dict]]:
    O = "rogue"
    a = [
        ability("shadow_stride", "Sneak", O, 1, "utility", [{"type": "sneak"}], "Toggle sneaking: move slowly and quietly, unnoticed by more creatures.",
                cd=1, gcd=False, target="self", skill="sneak"),
        ability("quick_jab", "Quick Jab", O, 1, "nuke", [{"type": "damage", "base": 3, "per_level": 1.0}], "A darting jab.", cost=3, cd=6, rng=4),
        ability("melt_away", "Hide", O, 3, "utility", [{"type": "hide"}], "Hide in the shadows. Moving ends it unless you are sneaking.",
                cd=2, gcd=False, target="self", skill="hide"),
        passive("rog_dodge", "Dodge", O, 4, "dodge", "Passive: a chance to dodge melee attacks."),
        ability("pick_lock", "Pick Lock", O, 6, "utility", [{"type": "pick_lock"}], "Work a lock open.", cd=5, gcd=False, rng=5, skill="pick_lock"),
        passive("rog_parry", "Parry", O, 12, "parry", "Passive: a chance to parry melee attacks."),
        passive("rog_dual", "Dual Wield", O, 13, "dual_wield", "Passive: swing a weapon in each hand."),
        passive("rog_double", "Double Attack", O, 16, "double_attack", "Passive: a chance to swing twice each round."),
        ability("blind_dust", "Blinding Dust", O, 19, "cc", [{"type": "stun", "duration": 2.5}], "Throw grit in your foe's eyes, stunning it.", cost=10, cd=20, rng=4),
        ability("evade", "Evade", O, 8, "utility", [{"type": "threat_drop", "value": 60}], "Slip out of your target's notice: it forgets 60% of its anger at you.",
                cost=6, cd=10, rng=10),
        ability("vanish_in_smoke", "Vanish in Smoke", O, 41, "utility", [{"type": "threat_drop", "value": 75, "radius": 30}],
                "Every foe within 30 meters forgets most of its anger at you.", cost=20, cd=120, target="self"),
        passive("rog_riposte", "Riposte", O, 30, "riposte", "Passive: a chance to riposte melee attacks."),
        ability("blade_flurry", "Blade Flurry", O, 35, "discipline",
                [{"type": "buff", "stat": "attack_speed_pct", "value": 30, "duration": 20}, {"type": "buff", "stat": "crit_chance_pct", "value": 10, "duration": 20}],
                "Offensive discipline: 30% faster attacks and more criticals for 20 seconds.", cost=25, cd=300, gcd=False, target="self"),
        ability("shadow_dance", "Shadow Dance", O, 44, "discipline", [{"type": "buff", "stat": "damage_taken_pct", "value": -40, "duration": 12}],
                "Defensive discipline: take 40% less melee damage for 12 seconds.", cost=20, cd=300, gcd=False, target="self"),
    ]
    a += ab_nuke(O, [("backstab", "Backstab", 10), ("deep_stab", "Deep Stab", 22), ("assassinate", "Assassinate", 38), ("deathblow", "Deathblow", 52)],
                 0.4, cost_per_level=0.3, cd=8, rng=4, skill="backstab", group="rogue_backstab", position="behind",
                 desc="Drive your blade into your target's back. You must be behind it. Rank {rank}.")
    for rank, lvl, nxt, sid, name in tiers([("serpent_strike", "Serpent Strike", 5), ("viper_strike", "Viper Strike", 25), ("adder_kiss", "Adder's Kiss", 46)]):
        t0, t1 = 0.25 * mob_hp(lvl), 0.24 * mob_hp(nxt)
        a.append(ability(sid, name, O, lvl, "dot", [dmg(0.06, 0.06, lvl, nxt), {"type": "dot", **scaled(t0 / 6, t1 / 6, lvl, nxt), "duration": 18, "tick": 3}],
                         f"A poisoned strike: the venom burns for 18 seconds. Rank {rank}.", cost=6 + 0.3 * lvl, cd=12, rng=4, stack_group="rogue_poison", rank=rank))
    a += [ability("hamstring", "Hamstring", O, 15, "debuff", [{"type": "snare", "value": 35, "duration": 12}], "Cut your foe's tendons: 35% slower for 12 seconds.",
                  cost=8, cd=15, rng=4),
          ability("gutting_cut", "Gutting Cut", O, 28, "nuke", [dmg(0.2, 0.18, 28, 60)], "A vicious slash to the gut.", cost=15, cd=10, rng=4),
          ability("crippling_poison", "Crippling Venom", O, 33, "debuff", [{"type": "slow", "value": 20, "duration": 30}], "A venom that slows your foe's attacks by 20%.",
                  cost=12, cd=30, rng=4),
          ability("mark_for_death", "Mark for Death", O, 49, "debuff", [{"type": "debuff", "stat": "ac", "value": -40, "duration": 30}], "Expose your target's weak points.",
                  cost=15, cd=30, rng=4),
          ability("garrote", "Garrote", O, 54, "cc", [{"type": "damage", "base": 10, "per_level": 1.0}, {"type": "stun", "duration": 3.0}],
                  "Choke your foe from behind, stunning it for 3 seconds.", cost=15, cd=20, rng=4, position="behind"),
          ability("thousand_cuts", "Thousand Cuts", O, 56, "discipline", [{"type": "buff", "stat": "melee_damage_pct", "value": 50, "duration": 20}],
                  "Offensive discipline: 50% harder blows for 20 seconds.", cost=30, cd=600, gcd=False, target="self"),
          ability("shadowstep_feint", "Feint", O, 58, "utility", [{"type": "threat_drop", "value": 90}], "A perfect feint: your target forgets almost all its anger at you.",
                  cost=10, cd=20, rng=10)]
    return a, []


# ------------------------------------------------------------------------------------------- monk

def monk() -> tuple[list[dict], list[dict]]:
    M = "monk"
    a = [
        passive("monk_fists", "Dual Wield", M, 1, "dual_wield", "Passive: both fists strike each round."),
        passive("monk_dodge", "Dodge", M, 1, "dodge", "Passive: a chance to dodge melee attacks."),
        ability("inner_mend", "Mend", M, 1, "heal", [{"type": "heal_pct", "value": 25}], "Center yourself and mend 25% of your health.", cd=360, gcd=False, target="self", skill="mend"),
        passive("monk_double", "Double Attack", M, 15, "double_attack", "Passive: a chance to swing twice each round."),
        ability("still_as_death", "Feign Death", M, 17, "utility", [{"type": "feign_death"}], "Fall as if dead. Your skill decides whether your foes believe it.",
                cost=0, cd=8, gcd=False, target="self", skill="feign_death"),
        passive("monk_enhanced", "Enhanced Damage", M, 20, "enhanced_damage", "Passive: your blows land harder."),
        passive("monk_riposte", "Riposte", M, 35, "riposte", "Passive: a chance to riposte melee attacks."),
        ability("leg_sweep", "Leg Sweep", M, 12, "cc", [{"type": "damage", "base": 3, "per_level": 0.6}, {"type": "stun", "duration": 2.0}],
                "Sweep your foe's legs out from under it.", cost=8, cd=14, rng=4),
        ability("stone_stance", "Stone Stance", M, 27, "discipline", [{"type": "buff", "stat": "damage_taken_pct", "value": -45, "duration": 12}],
                "Defensive discipline: take 45% less melee damage for 12 seconds.", cost=20, cd=300, gcd=False, target="self"),
        ability("thunderfist", "Thunderfist", M, 39, "discipline", [{"type": "buff", "stat": "melee_damage_pct", "value": 40, "duration": 20}],
                "Offensive discipline: 40% harder blows for 20 seconds.", cost=25, cd=300, gcd=False, target="self"),
        ability("calm_mind", "Calm Mind", M, 31, "heal", [{"type": "restore_endurance_pct", "value": 50}], "Breathe deep and recover half your endurance.", cd=300, gcd=False, target="self"),
        ability("serene_mend", "Serene Mend", M, 45, "heal", [{"type": "heal_pct", "value": 50}], "A deeper mend: 50% of your health.", cd=360, gcd=False, target="self", skill="mend"),
        ability("hundred_hands", "Hundred Hands", M, 55, "discipline", [{"type": "buff", "stat": "attack_speed_pct", "value": 50, "duration": 15}],
                "Offensive discipline: 50% faster strikes for 15 seconds.", cost=30, cd=600, gcd=False, target="self"),
        ability("palm_of_calm", "Palm of Calm", M, 48, "utility", [{"type": "threat_drop", "value": 50}], "A soothing palm strike: your foe forgets half its anger at you.",
                cost=10, cd=20, rng=4),
    ]
    a += ab_nuke(M, [("spinning_heel", "Spinning Heel", 3), ("leopard_claw", "Leopard Claw", 9), ("hawk_strike", "Hawk Strike", 18), ("dragon_palm", "Dragon Palm", 30),
                     ("soaring_kick", "Soaring Kick", 42), ("phoenix_kick", "Phoenix Kick", 57)],
                 0.18, cost_per_level=0.25, cd=7, rng=4, skill="kick", group="monk_kick", desc="A trained strike. Rank {rank} of the monk's special attacks.")
    a += [ability("crane_strike", "Crane Strike", M, 6, "nuke", [dmg(0.1, 0.09, 6, 60)], "A swift beak-hand strike.", cost=5, cd=10, rng=4),
          ability("dragon_breath", "Inner Fire", M, 24, "nuke", [dmg(0.15, 0.14, 24, 60)], "Strike with focused inner fire.", cost=14, cd=12, rng=4),
          ability("pressure_point", "Pressure Point", M, 36, "debuff", [{"type": "slow", "value": 20, "duration": 20}], "Strike a nerve: your foe attacks 20% slower.",
                  cost=12, cd=25, rng=4),
          ability("whirlwind_strike", "Whirlwind Strike", M, 22, "nuke", [dmg(0.12, 0.11, 22, 60)], "A spinning strike.", cost=10, cd=10, rng=4),
          ability("iron_body", "Iron Body", M, 33, "discipline", [{"type": "buff", "stat": "ac", "value": 40, "duration": 30}],
                  "Harden your body: +40 armor for 30 seconds.", cost=15, cd=180, gcd=False, target="self"),
          ability("tiger_spirit", "Spirit of the Tiger", M, 53, "discipline", [{"type": "buff", "stat": "crit_chance_pct", "value": 15, "duration": 30}],
                  "Offensive discipline: more critical strikes for 30 seconds.", cost=20, cd=300, gcd=False, target="self"),
          ability("crushing_palm", "Crushing Palm", M, 51, "debuff", [{"type": "debuff", "stat": "ac", "value": -35, "duration": 30}], "Shatter your foe's guard.",
                  cost=14, cd=30, rng=4)]
    return a, []


# ------------------------------------------------------------------------------------------- druid

def druid() -> tuple[list[dict], list[dict]]:
    D = "druid"
    a = [ability("druid_track", "Wildsense", D, 20, "utility", [{"type": "track", "radius": 120, "value": 2}], "Sense the creatures around you.",
                 cd=5, gcd=False, target="self", skill="tracking")]
    s: list[dict] = []
    s += sp_heal(D, [("mossmend", "Mossmend", 1), ("sapling_balm", "Sapling Balm", 11), ("heartwood_mend", "Heartwood Mend", 24), ("grove_restoration", "Grove Restoration", 39),
                     ("wildheart_renewal", "Wildheart Renewal", 54)], 0.38, 1.8, group="druid_heal", desc="Nature mends your target. Rank {rank} of the druid heal.")
    for rank, lvl, nxt, sid, name in tiers([("spring_rain", "Spring Rain", 5), ("summer_rain", "Summer Rain", 21), ("monsoon_bloom", "Monsoon Bloom", 37),
                                             ("eternal_spring", "Eternal Spring", 51)]):
        t0, t1 = 0.7 * class_hp("warrior", lvl), 0.65 * class_hp("warrior", nxt)
        s.append(spell(sid, name, D, lvl, "hot", t0 / 2.6, 2.5, 0, 60, "friendly", "none", [{"type": "hot", **scaled(t0 / 8, t1 / 8, lvl, nxt), "duration": 24, "tick": 3}],
                       "Gentle rain heals your target every 3 seconds for 24 seconds.", stack_group="druid_hot", rank=rank))
    s += sp_nuke(D, [("sunspark", "Sunspark", 1), ("wildfire", "Wildfire", 15), ("sunstrike", "Sunstrike", 33), ("solar_flare", "Solar Flare", 49)],
                 0.33, "fire", 1.2, group="druid_fire", desc="Call the sun's fire upon your target. Rank {rank}.")
    s += sp_dot(D, [("stinging_swarm", "Stinging Swarm", 3), ("locust_cloud", "Locust Cloud", 19), ("plague_of_wasps", "Plague of Wasps", 35), ("devouring_swarm", "Devouring Swarm", 52)],
                0.55, "poison", 1.4, group="druid_swarm", desc="Insects sting your target for {dur} seconds. Rank {rank}.")
    s += [spell("tangle_roots", "Tangle Roots", D, 4, "cc", 12, 1.5, 0, 50, "enemy", "magic", [{"type": "root", "duration": 20, "max_target_level": 24}],
                "Roots hold your target in place. No effect above level 24.", stack_group="druid_root", rank=1),
          spell("strangling_vines", "Strangling Vines", D, 28, "cc", 40, 1.5, 0, 50, "enemy", "magic", [{"type": "root", "duration": 30, "max_target_level": 50}],
                "Stronger roots. No effect above level 50.", stack_group="druid_root", rank=2),
          spell("mire_grasp", "Mire Grasp", D, 7, "debuff", 15, 1.5, 0, 50, "enemy", "magic", [{"type": "snare", "value": 45, "duration": 40}],
                "Mud drags at your target: 45% slower for 40 seconds.")]
    s += sp_buff(D, [("barkskin", "Barkskin", 2, {"ac": 4}), ("stoneskin_bark", "Stoneskin Bark", 17, {"ac": 10, "max_hp": 40}),
                     ("ironbark", "Ironbark", 36, {"ac": 18, "max_hp": 100}), ("heartwood_aegis", "Heartwood Aegis", 55, {"ac": 26, "max_hp": 190})],
                 group="druid_skin", desc="Your target's skin hardens like bark: {stats} for {minutes} minutes.")
    s += sp_buff(D, [("thornskin", "Thornskin", 9, {"damage_shield": 3}), ("briarskin", "Briarskin", 26, {"damage_shield": 8}), ("bramble_mantle", "Bramble Mantle", 46, {"damage_shield": 15})],
                 group="druid_ds", dur=600, desc="Thorns cover your target: attackers take damage ({stats}) for {minutes} minutes.")
    s += [spell("stag_spirit", "Spirit of the Stag", D, 13, "buff", 30, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "move_speed_pct", "value": 35, "duration": 1800}],
                "Your target runs 35% faster for 30 minutes."),
          spell("druid_gate", "Return to the Grove", D, 10, "utility", 25, 5.0, 60, 0, "self", "none", [{"type": "teleport", "destination": "bind"}], "Return to your bind point."),
          spell("stonepath_emberhold", "Stonepath to Emberhold", D, 23, "utility", 50, 6.0, 60, 0, "group", "none", [{"type": "teleport", "destination": "emberhold"}],
                "Travel through the standing stones to Emberhold."),
          spell("stonepath_thornwood", "Stonepath to Thornwood", D, 25, "utility", 50, 6.0, 60, 0, "group", "none", [{"type": "teleport", "destination": "thornwood"}],
                "Travel through the standing stones to Thornwood."),
          spell("stonepath_cragmouth", "Stonepath to Cragmouth", D, 27, "utility", 50, 6.0, 60, 0, "group", "none", [{"type": "teleport", "destination": "cragmouth"}],
                "Travel through the standing stones to Cragmouth."),
          spell("ringwalk_emberglade", "Ringwalk: Emberglade", D, 12, "utility", 50, 6.0, 60, 0, "group", "none",
                [{"type": "teleport", "destination": "ring_emberglade"}], "Step through the standing stones to the druid ring in the Emberglade."),
          spell("ringwalk_briarwatch", "Ringwalk: Briarwatch", D, 14, "utility", 50, 6.0, 60, 0, "group", "none",
                [{"type": "teleport", "destination": "ring_briarwatch"}], "Step through the standing stones to the druid ring in Briarwatch."),
          spell("ringwalk_hollowpine", "Ringwalk: Hollowpine", D, 24, "utility", 50, 6.0, 60, 0, "group", "none",
                [{"type": "teleport", "destination": "ring_hollowpine"}], "Step through the standing stones to the druid ring in Hollowpine."),
          spell("ringwalk_frostveil", "Ringwalk: Frostveil", D, 38, "utility", 50, 6.0, 60, 0, "group", "none",
                [{"type": "teleport", "destination": "ring_frostveil"}], "Step through the standing stones to the druid ring in Frostveil."),
          spell("ringwalk_ashen", "Ringwalk: Ashen Reach", D, 46, "utility", 50, 6.0, 60, 0, "group", "none",
                [{"type": "teleport", "destination": "ring_ashen"}], "Step through the standing stones to the druid ring in the Ashen Reach."),
          spell("shroud_of_leaves", "Shroud of Leaves", D, 30, "buff", 40, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "invisible", "value": 1, "duration": 600}],
                "Hide your target among the leaves: creatures cannot see it for 10 minutes, until it acts."),
          spell("cleanse_sap", "Cleansing Sap", D, 14, "utility", 25, 2.0, 0, 40, "friendly", "none", [{"type": "dispel", "value": 2}], "Cleanse two harmful effects."),
          spell("call_lightning", "Call Lightning", D, 42, "nuke", 140, 3.0, 8, 60, "area", "magic", [dmg(0.25, 0.24, 42, 60)],
                "Lightning strikes your target and every foe within 8 meters of it.", radius=8),
          spell("natures_ward", "Nature's Ward", D, 44, "buff", 90, 3.0, 0, 60, "friendly", "none",
                [{"type": "buff", "stat": "resist_fire", "value": 30, "duration": 1800}, {"type": "buff", "stat": "resist_cold", "value": 30, "duration": 1800}],
                "+30 fire and cold resist for 30 minutes."),
          spell("wild_regrowth", "Wild Regrowth", D, 58, "heal", 200, 3.0, 0, 60, "group", "none", [{"type": "heal_pct", "value": 40}],
                "A burst of growth heals you and your companions for 40% of their health.")]
    return a, s


# ------------------------------------------------------------------------------------------- shaman

def shaman() -> tuple[list[dict], list[dict]]:
    S = "shaman"
    a: list[dict] = []
    s: list[dict] = []
    s += sp_heal(S, [("spirit_salve", "Spirit Salve", 1), ("ancestral_mending", "Ancestral Mending", 14), ("totem_restoration", "Totem Restoration", 30),
                     ("spirit_renewal", "Spirit Renewal", 47)], 0.34, 1.7, group="shaman_heal", desc="The spirits mend your target. Rank {rank}.")
    for rank, (sid, name, lvl, v, cap) in enumerate([("weary_spirit", "Weary Spirit", 5, 20, 30), ("heavy_limbs", "Heavy Limbs", 18, 30, 45),
                                                       ("leaden_soul", "Leaden Soul", 33, 40, 58), ("sapping_spirit", "Sapping Spirit", 49, 55, 65)], 1):
        s.append(spell(sid, name, S, lvl, "debuff", 10 + lvl * 2, 2.5, 0, 50, "enemy", "magic", [{"type": "slow", "value": v, "duration": 120, "max_target_level": cap}],
                       f"The spirits weigh down your target: it attacks {v}% slower for 2 minutes.", stack_group="shaman_slow", rank=rank))
    for rank, (sid, name, lvl, v) in enumerate([("spirit_quickening", "Spirit Quickening", 12, 15), ("wind_of_haste", "Wind of Haste", 28, 25),
                                                 ("ancestral_alacrity", "Ancestral Alacrity", 45, 35)], 1):
        s.append(spell(sid, name, S, lvl, "buff", 20 + lvl * 2, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "attack_speed_pct", "value": v, "duration": 900}],
                       f"Quicken your target: {v}% faster attacks for 15 minutes.", stack_group="shaman_haste", rank=rank))
    s += sp_buff(S, [("bear_strength", "Strength of the Bear", 2, {"str": 8}), ("ox_strength", "Strength of the Ox", 20, {"str": 20}),
                     ("titan_strength", "Strength of the Titan", 40, {"str": 35})], group="shaman_str", desc="{stats} for {minutes} minutes.")
    s += sp_buff(S, [("tortoise_spirit", "Spirit of the Tortoise", 6, {"sta": 8, "max_hp": 20}), ("mountain_spirit", "Spirit of the Mountain", 24, {"sta": 20, "max_hp": 70}),
                     ("ancient_stone", "Ancient Stone", 43, {"sta": 35, "max_hp": 150})], group="shaman_sta", desc="{stats} for {minutes} minutes.")
    s += sp_buff(S, [("cat_grace", "Grace of the Cat", 10, {"agi": 10, "ac": 4}), ("lynx_grace", "Grace of the Lynx", 34, {"agi": 25, "ac": 10})],
                 group="shaman_agi", desc="{stats} for {minutes} minutes.")
    s += sp_dot(S, [("bitter_sickness", "Bitter Sickness", 3), ("fevered_blood", "Fevered Blood", 22), ("spirit_plague", "Spirit Plague", 39), ("soul_rot", "Soul Rot", 55)],
                0.5, "disease", 1.4, group="shaman_disease", desc="A wasting sickness for {dur} seconds. Rank {rank}.")
    s += sp_dot(S, [("serpent_venom", "Serpent Venom", 8), ("spider_venom", "Spider Venom", 26), ("basilisk_venom", "Basilisk Venom", 44)],
                0.45, "poison", 1.4, group="shaman_poison", desc="Venom burns for {dur} seconds. Rank {rank}.")
    s += sp_nuke(S, [("frost_spirit", "Frost Spirit", 1), ("winter_spirit", "Winter Spirit", 16), ("glacial_spirit", "Glacial Spirit", 36), ("rime_ancestor", "Rime Ancestor", 52)],
                 0.28, "cold", 1.1, group="shaman_cold", desc="A spirit of frost strikes your target. Rank {rank}.")
    s += sp_pet(S, [("spirit_companion", "Spirit Companion", 30, 30), ("great_spirit_wolf", "Great Spirit Wolf", 50, 46)], "spirit_wolf", group="shaman_pet",
                desc="Call a spirit wolf to hunt beside you (up to level {cap}).")
    s += [spell("spirit_offering", "Spirit Offering", S, 23, "utility", 0, 0.0, 10, 0, "self", "none", [{"type": "hp_to_mana", "base": 10, "per_level": 2.0}],
                "Trade your own health for mana.", stack_group="shaman_canni", rank=1),
          spell("deep_offering", "Deep Offering", S, 42, "utility", 0, 0.0, 10, 0, "self", "none", [{"type": "hp_to_mana", "base": 20, "per_level": 3.5}],
                "Trade more health for mana.", stack_group="shaman_canni", rank=2),
          spell("spirit_sight", "Spirit Regeneration", S, 29, "buff", 50, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "hp_regen", "value": 5, "duration": 600}],
                "+5 health regeneration per second for 10 minutes."),
          spell("shaman_gate", "Spirit Walk", S, 11, "utility", 25, 5.0, 60, 0, "self", "none", [{"type": "teleport", "destination": "bind"}], "Walk the spirit path to your bind point."),
          spell("cure_sickness", "Cure Sickness", S, 15, "utility", 25, 2.0, 0, 40, "friendly", "none", [{"type": "dispel", "value": 2}], "Remove two harmful effects."),
          spell("spirit_wither", "Spirit Wither", S, 37, "debuff", 60, 2.0, 0, 50, "enemy", "magic",
                [{"type": "debuff", "stat": "resist_all", "value": -25, "duration": 90}], "Lower your target's resists by 25 for 90 seconds."),
          spell("ancestral_guard", "Ancestral Guard", S, 58, "buff", 150, 3.0, 0, 60, "friendly", "none",
                [{"type": "buff", "stat": "max_hp", "value": 250, "duration": 1800}, {"type": "buff", "stat": "ac", "value": 20, "duration": 1800}],
                "The ancestors guard your target: +250 health and +20 armor for 30 minutes.")]
    return a, s


# ------------------------------------------------------------------------------------------- magician

def magician() -> tuple[list[dict], list[dict]]:
    M = "magician"
    s: list[dict] = []
    for pid, label, ranks in [("ember_servant", "Fire", [("call_ember_servant", "Call Ember Servant", 1, 8), ("call_blaze_servant", "Call Blaze Servant", 21, 26),
                                                          ("call_inferno_servant", "Call Inferno Servant", 41, 46)]),
                              ("tide_servant", "Water", [("call_tide_servant", "Call Tide Servant", 5, 12), ("call_flood_servant", "Call Flood Servant", 25, 30),
                                                         ("call_maelstrom_servant", "Call Maelstrom Servant", 45, 50)]),
                              ("stone_servant", "Earth", [("call_stone_servant", "Call Stone Servant", 9, 16), ("call_granite_servant", "Call Granite Servant", 29, 34),
                                                          ("call_mountain_servant", "Call Mountain Servant", 49, 56)]),
                              ("gale_servant", "Air", [("call_gale_servant", "Call Gale Servant", 13, 20), ("call_storm_servant", "Call Storm Servant", 33, 38),
                                                       ("call_tempest_servant", "Call Tempest Servant", 53, 60)])]:
        s += sp_pet(M, ranks, pid, group="mage_" + pid, desc=label + " servant: summon an elemental that fights for you (up to level {cap}).")
    s += sp_nuke(M, [("flame_bolt", "Flame Bolt", 1), ("searing_ray", "Searing Ray", 12), ("molten_lance", "Molten Lance", 27), ("starfire", "Starfire", 42),
                     ("sunburst", "Sunburst", 57)], 0.32, "fire", 1.25, group="mage_fire", desc="A bolt of conjured fire. Rank {rank}.")
    s += sp_nuke(M, [("force_shard", "Force Shard", 7), ("arcane_lash", "Arcane Lash", 24), ("rending_force", "Rending Force", 44)], 0.3, "magic", 1.2,
                 group="mage_magic", desc="A shard of raw force. Rank {rank}.")
    s += sp_buff(M, [("burning_mantle", "Burning Mantle", 3, {"damage_shield": 3}), ("flame_mantle", "Flame Mantle", 19, {"damage_shield": 7}),
                     ("cinder_mantle", "Cinder Mantle", 35, {"damage_shield": 13}), ("solar_mantle", "Solar Mantle", 51, {"damage_shield": 20})],
                 group="mage_ds", dur=600, desc="Flames dance on your target: attackers take damage ({stats}) for {minutes} minutes.")
    s += [spell("conjure_bread", "Conjure Bread", M, 2, "utility", 10, 3.0, 0, 0, "self", "none", [{"type": "summon_item", "item": "conjured_bread", "value": 4, "duration": 3600}],
                "Conjure four loaves of bread. They fade after an hour."),
          spell("conjure_water", "Conjure Water", M, 4, "utility", 10, 3.0, 0, 0, "self", "none", [{"type": "summon_item", "item": "conjured_water", "value": 4, "duration": 3600}],
                "Conjure four flasks of water. They fade after an hour."),
          spell("conjure_blade", "Conjure Blade", M, 16, "utility", 40, 5.0, 0, 0, "self", "none", [{"type": "summon_item", "item": "conjured_blade", "value": 1, "duration": 1800}],
                "Conjure a blade of solid flame for a companion. It fades after 30 minutes."),
          spell("mend_servant", "Mend Servant", M, 11, "heal", 25, 3.0, 0, 60, "pet", "none", [{"type": "heal", **heal_amt(0.5, 0.45, 11, 60)}],
                "Mend your elemental servant.", stack_group="mage_pet_heal", rank=1),
          spell("renew_servant", "Renew Servant", M, 38, "heal", 90, 3.0, 0, 60, "pet", "none", [{"type": "heal", **heal_amt(0.5, 0.45, 38, 60)}],
                "Greatly mend your servant.", stack_group="mage_pet_heal", rank=2),
          spell("elemental_fury", "Elemental Fury", M, 23, "buff", 50, 3.0, 0, 60, "pet", "none",
                [{"type": "buff", "stat": "attack_speed_pct", "value": 25, "duration": 600}, {"type": "buff", "stat": "melee_damage_pct", "value": 15, "duration": 600}],
                "Your servant attacks 25% faster and 15% harder for 10 minutes."),
          spell("weaken_wards", "Weaken Wards", M, 31, "debuff", 60, 2.0, 0, 50, "enemy", "magic", [{"type": "debuff", "stat": "resist_fire", "value": -30, "duration": 90}],
                "Lower your target's fire resist by 30 for 90 seconds."),
          spell("rain_of_cinders", "Rain of Cinders", M, 47, "nuke", 200, 3.0, 10, 60, "area", "fire", [dmg(0.22, 0.2, 47, 60)],
                "Cinders rain on your target and every foe within 10 meters of it.", radius=10),
          spell("elemental_rage", "Elemental Rage", M, 55, "buff", 120, 3.0, 0, 60, "pet", "none",
                [{"type": "buff", "stat": "attack_speed_pct", "value": 40, "duration": 600}, {"type": "buff", "stat": "melee_damage_pct", "value": 25, "duration": 600}],
                "Your servant attacks 40% faster and 25% harder for 10 minutes."),
          spell("mage_gate", "Elemental Return", M, 18, "utility", 25, 5.0, 60, 0, "self", "none", [{"type": "teleport", "destination": "bind"}], "Return to your bind point."),
          spell("planar_shield", "Planar Shield", M, 15, "buff", 30, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "ac", "value": 8, "duration": 1800}, {"type": "buff", "stat": "max_hp", "value": 40, "duration": 1800}],
                "A shimmering planar shield: +8 armor and +40 health for 30 minutes.", stack_group="mage_shield", rank=1),
          spell("planar_bulwark", "Planar Bulwark", M, 39, "buff", 70, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "ac", "value": 18, "duration": 1800}, {"type": "buff", "stat": "max_hp", "value": 130, "duration": 1800}],
                "+18 armor and +130 health for 30 minutes.", stack_group="mage_shield", rank=2)]
    return [], s


# ------------------------------------------------------------------------------------------- necromancer

def necromancer() -> tuple[list[dict], list[dict]]:
    N = "necromancer"
    s: list[dict] = []
    s += sp_pet(N, [("raise_bone_thrall", "Raise Bone Thrall", 3, 8), ("raise_ghoul", "Raise Ghoul", 13, 18), ("raise_grave_warden", "Raise Grave Warden", 23, 28),
                    ("raise_crypt_lord", "Raise Crypt Lord", 33, 38), ("raise_bone_colossus", "Raise Bone Colossus", 43, 48),
                    ("raise_deathless_king", "Raise Deathless King", 53, 58)], "bone_thrall", group="necro_pet",
                desc="Raise a skeletal servant from the dust (up to level {cap}).")
    s += sp_dot(N, [("creeping_blight", "Creeping Blight", 1), ("bone_rot", "Bone Rot", 10), ("marrow_decay", "Marrow Decay", 25), ("grave_pestilence", "Grave Pestilence", 40),
                    ("doom_plague", "Doom Plague", 55)], 0.6, "disease", 1.5, group="necro_disease", desc="Decay eats your target for {dur} seconds. Rank {rank}.")
    s += sp_dot(N, [("venom_mist", "Venom Mist", 6), ("black_venom", "Black Venom", 21), ("deathcap_toxin", "Deathcap Toxin", 36), ("nightshade_essence", "Nightshade Essence", 51)],
                0.55, "poison", 1.5, group="necro_poison", desc="Poison seeps into your target for {dur} seconds. Rank {rank}.")
    s += sp_dot(N, [("soul_siphon", "Soul Siphon", 8), ("life_drinker", "Life Drinker", 28), ("essence_drain", "Essence Drain", 48)], 0.4, "magic", 1.3,
                group="necro_drain", kind="drain", value=100, desc="Drain your target's life into your own for {dur} seconds. Rank {rank}.")
    for rank, lvl, nxt, sid, name in tiers([("grave_touch_tap", "Leech Touch", 1), ("vampiric_kiss", "Vampiric Kiss", 17), ("soul_harvest", "Soul Harvest", 38)]):
        h0, h1 = 0.22 * mob_hp(lvl), 0.21 * mob_hp(nxt)
        s.append(spell(sid, name, N, lvl, "nuke", h0 / (1.3 + 0.01 * lvl), 2.0, 0, 50, "enemy", "magic", [{"type": "lifetap", **scaled(h0, h1, lvl, nxt), "value": 100}],
                       f"Steal your target's life for your own. Rank {rank}.", stack_group="necro_tap", rank=rank))
    s += [spell("deathly_stillness", "Deathly Stillness", N, 15, "utility", 20, 0.0, 30, 0, "self", "none", [{"type": "feign_death", "value": 90}],
                "Fall as if dead (90% chance to fool your foes).", stack_group="necro_feign", rank=1),
          spell("dark_bargain", "Dark Bargain", N, 19, "utility", 0, 0.0, 10, 0, "self", "none", [{"type": "hp_to_mana", "base": 15, "per_level": 2.5}],
                "Bargain away some of your life for mana.", stack_group="necro_bargain", rank=1),
          spell("grim_bargain", "Grim Bargain", N, 45, "utility", 0, 0.0, 10, 0, "self", "none", [{"type": "hp_to_mana", "base": 30, "per_level": 4.0}],
                "A grimmer bargain: more life for more mana.", stack_group="necro_bargain", rank=2),
          spell("clutching_dead", "Clutching Dead", N, 4, "debuff", 12, 1.5, 0, 50, "enemy", "disease", [{"type": "snare", "value": 45, "duration": 40}],
                "Dead hands clutch at your target: 45% slower for 40 seconds."),
          spell("graveyard_terror", "Graveyard Terror", N, 12, "cc", 25, 1.5, 10, 40, "enemy", "magic", [{"type": "fear", "duration": 12, "max_target_level": 30}],
                "Fill your target with dread: it flees for 12 seconds. No effect above level 30.", stack_group="necro_fear", rank=1),
          spell("dread_of_the_tomb", "Dread of the Tomb", N, 34, "cc", 60, 1.5, 10, 40, "enemy", "magic", [{"type": "fear", "duration": 15, "max_target_level": 55}],
                "A stronger fear. No effect above level 55.", stack_group="necro_fear", rank=2),
          spell("mend_bones", "Mend Bones", N, 14, "heal", 25, 3.0, 0, 60, "pet", "none", [{"type": "heal", **heal_amt(0.5, 0.45, 14, 60)}],
                "Knit your undead servant's bones."),
          spell("dark_vigor", "Dark Vigor", N, 31, "buff", 60, 3.0, 0, 60, "pet", "none",
                [{"type": "buff", "stat": "attack_speed_pct", "value": 20, "duration": 600}, {"type": "buff", "stat": "ac", "value": 15, "duration": 600}],
                "Your servant attacks 20% faster with +15 armor for 10 minutes."),
          spell("bone_shield", "Bone Shield", N, 2, "buff", 10, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "ac", "value": 4, "duration": 1800}, {"type": "buff", "stat": "max_hp", "value": 12, "duration": 1800}],
                "Bones knit into armor around you: +4 armor and +12 health for 30 minutes.", stack_group="necro_shield", rank=1),
          spell("lich_shroud", "Lich Shroud", N, 30, "buff", 60, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "mana_regen", "value": 4, "duration": 1800}, {"type": "buff", "stat": "ac", "value": 14, "duration": 1800}],
                "Take on a deathly aspect: +4 mana regeneration and +14 armor for 30 minutes.", stack_group="necro_shield", rank=2),
          spell("lich_form", "Lich Form", N, 57, "buff", 150, 3.0, 0, 0, "self", "none",
                [{"type": "buff", "stat": "mana_regen", "value": 9, "duration": 1800}, {"type": "buff", "stat": "ac", "value": 25, "duration": 1800}],
                "Become half-lich: +9 mana regeneration and +25 armor for 30 minutes.", stack_group="necro_shield", rank=3),
          spell("necro_gate", "Shadow Return", N, 9, "utility", 25, 5.0, 60, 0, "self", "none", [{"type": "teleport", "destination": "bind"}], "Return to your bind point."),
          spell("withering_curse", "Withering Curse", N, 42, "debuff", 70, 2.0, 0, 50, "enemy", "disease",
                [{"type": "debuff", "stat": "melee_damage_pct", "value": -20, "duration": 60}], "Your target hits 20% softer for a minute."),
          spell("death_veil", "Veil of Death", N, 26, "buff", 40, 3.0, 0, 0, "self", "none", [{"type": "buff", "stat": "invisible", "value": 1, "duration": 600}],
                "The living cannot see you for 10 minutes, until you act.")]
    return [], s


# ------------------------------------------------------------------------------------------- enchanter

def enchanter() -> tuple[list[dict], list[dict]]:
    E = "enchanter"
    s: list[dict] = []
    for rank, (sid, name, lvl, dur, cap) in enumerate([("soothing_trance", "Soothing Trance", 1, 18, 12), ("mind_lull", "Mind Lull", 8, 24, 24),
                                                        ("dazzling_trance", "Dazzling Trance", 20, 30, 38), ("deep_trance", "Deep Trance", 34, 36, 52),
                                                        ("captivating_gaze", "Captivating Gaze", 48, 42, 62)], 1):
        s.append(spell(sid, name, E, lvl, "cc", 8 + lvl * 1.5, 1.5, 0, 50, "enemy", "magic", [{"type": "mez", "duration": dur, "max_target_level": cap}],
                       f"Lull your target into a trance for {dur} seconds. Damage wakes it. No effect above level {cap}.", stack_group="ench_mez", rank=rank))
    for rank, (sid, name, lvl, dur, cap) in enumerate([("coax_will", "Coax Will", 12, 90, 16), ("captivate_will", "Captivate Will", 24, 120, 30),
                                                        ("bend_will", "Bend Will", 37, 150, 44), ("absolute_command", "Absolute Command", 52, 180, 58)], 1):
        s.append(spell(sid, name, E, lvl, "cc", 15 + lvl * 2, 3.0, 0, 40, "enemy", "magic", [{"type": "charm", "duration": dur, "max_target_level": cap}],
                       f"Bend your target to your will for up to {dur} seconds; it may break free sooner. No effect above level {cap}.", stack_group="ench_charm", rank=rank,
                       resist_adjust=-10))
    for rank, (sid, name, lvl, v) in enumerate([("quickened_mind", "Quickened Mind", 15, 20), ("swift_thought", "Swift Thought", 29, 30), ("lightning_wit", "Lightning Wit", 46, 40)], 1):
        s.append(spell(sid, name, E, lvl, "buff", 20 + lvl * 2, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "attack_speed_pct", "value": v, "duration": 900}],
                       f"Quicken your target: {v}% faster attacks for 15 minutes.", stack_group="ench_haste", rank=rank))
    for rank, (sid, name, lvl, v) in enumerate([("mind_spring", "Mind Spring", 5, 1), ("clear_thought", "Clear Thought", 21, 3), ("tranquil_wellspring", "Tranquil Wellspring", 41, 6)], 1):
        s.append(spell(sid, name, E, lvl, "buff", 15 + lvl * 2, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "mana_regen", "value": v, "duration": 1800}],
                       f"Clear your target's mind: +{v} mana regeneration per second for 30 minutes.", stack_group="ench_mana", rank=rank))
    for rank, (sid, name, lvl, v, cap) in enumerate([("leaden_mind", "Leaden Mind", 10, 20, 30), ("sluggish_thought", "Sluggish Thought", 27, 35, 48), ("forgetful_haze", "Forgetful Haze", 44, 50, 63)], 1):
        s.append(spell(sid, name, E, lvl, "debuff", 10 + lvl * 2, 2.5, 0, 50, "enemy", "magic", [{"type": "slow", "value": v, "duration": 120, "max_target_level": cap}],
                       f"Your target attacks {v}% slower for 2 minutes.", stack_group="ench_slow", rank=rank))
    s += sp_nuke(E, [("mind_spike", "Mind Spike", 1), ("psychic_lash", "Psychic Lash", 17), ("shatter_will", "Shatter Will", 39)], 0.25, "magic", 1.0,
                 group="ench_nuke", desc="A spike of psychic force. Rank {rank}.")
    s += sp_pet(E, [("animate_blade", "Animate Blade", 16, 20), ("animate_armor", "Animate Armor", 40, 44)], "animated_armor", group="ench_pet",
                desc="Animate an empty suit of armor to fight for you (up to level {cap}).")
    s += [spell("prismatic_flash", "Prismatic Flash", E, 3, "cc", 12, 1.0, 12, 0, "area", "magic", [{"type": "stun", "duration": 2.0, "max_target_level": 30}],
                "A burst of color stuns every foe within 10 meters. No effect above level 30.", radius=10),
          spell("unravel_wards", "Unravel Wards", E, 14, "debuff", 25, 1.5, 0, 50, "enemy", "magic", [{"type": "debuff", "stat": "resist_magic", "value": -30, "duration": 120}],
                "Lower your target's magic resist by 30 for 2 minutes."),
          spell("fading_memory", "Fading Memory", E, 19, "utility", 25, 1.5, 0, 50, "enemy", "magic", [{"type": "threat_drop", "value": 100}],
                "Your target forgets everything it holds against you."),
          spell("mask_of_stone", "Mask of Stone", E, 7, "utility", 15, 3.0, 0, 0, "self", "none", [{"type": "illusion", "model": "deepkin", "duration": 1800}],
                "Take on the look of a Deepkin for 30 minutes.", stack_group="ench_illusion", rank=1),
          spell("mask_of_starlight", "Mask of Starlight", E, 11, "utility", 15, 3.0, 0, 0, "self", "none", [{"type": "illusion", "model": "vaelith", "duration": 1800}],
                "Take on the look of a Vaelith for 30 minutes.", stack_group="ench_illusion", rank=1),
          spell("guise_of_bones", "Guise of Bones", E, 23, "utility", 25, 3.0, 0, 0, "self", "none", [{"type": "illusion", "model": "skeleton", "duration": 1800}],
                "Look like a walking skeleton for 30 minutes.", stack_group="ench_illusion", rank=1),
          spell("guise_of_the_wolf", "Guise of the Wolf", E, 31, "utility", 25, 3.0, 0, 0, "self", "none", [{"type": "illusion", "model": "wolf", "duration": 1800}],
                "Take the shape of a wolf for 30 minutes.", stack_group="ench_illusion", rank=1),
          spell("mind_ward", "Mind Ward", E, 2, "buff", 10, 3.0, 0, 0, "self", "none", [{"type": "buff", "stat": "damage_taken_pct", "value": -10, "duration": 600}],
                "A ward of will: take 10% less melee damage for 10 minutes.", stack_group="ench_ward", rank=1),
          spell("mind_bulwark", "Mind Bulwark", E, 33, "buff", 60, 3.0, 0, 0, "self", "none", [{"type": "buff", "stat": "damage_taken_pct", "value": -25, "duration": 600}],
                "Take 25% less melee damage for 10 minutes.", stack_group="ench_ward", rank=2),
          spell("fade_from_sight", "Fade from Sight", E, 26, "buff", 30, 3.0, 0, 60, "friendly", "none", [{"type": "buff", "stat": "invisible", "value": 1, "duration": 600}],
                "Your target fades from sight for 10 minutes, until it acts."),
          spell("bewildering_lights", "Bewildering Lights", E, 36, "cc", 70, 2.0, 20, 0, "area", "magic", [{"type": "mez", "duration": 18, "max_target_level": 52}],
                "Mesmerize every foe within 12 meters. No effect above level 52.", radius=12),
          spell("tash_of_minds", "Sunder Resolve", E, 50, "debuff", 80, 2.0, 0, 50, "enemy", "magic", [{"type": "debuff", "stat": "resist_all", "value": -35, "duration": 120}],
                "Lower all your target's resists by 35 for 2 minutes."),
          spell("mind_gate", "Mindwalk Return", E, 9, "utility", 25, 5.0, 60, 0, "self", "none", [{"type": "teleport", "destination": "bind"}], "Return to your bind point."),
          spell("stun_of_thought", "Thoughtlock", E, 55, "cc", 70, 1.0, 12, 50, "enemy", "magic", [{"type": "stun", "duration": 4.0, "max_target_level": 62}],
                "Lock your target's thoughts: stunned for 4 seconds."),
          spell("grand_illusion", "Grand Illusion", E, 58, "utility", 30, 3.0, 0, 0, "self", "none", [{"type": "illusion", "model": "drake", "duration": 1800}],
                "Take the shape of a drake for 30 minutes.", stack_group="ench_illusion", rank=1)]
    return [], s


# ------------------------------------------------------------------------------------------- Phase 4 additions to Phase 3 classes

def cleric_extras() -> list[dict]:
    C = "cleric"
    return [spell("shining_resolve", "Shining Resolve", C, 39, "buff", 90, 3.0, 0, 60, "friendly", "none",
                  [{"type": "buff", "stat": "resist_all", "value": 20, "duration": 1800}], "+20 to all resists for 30 minutes."),
            spell("mark_of_retribution", "Mark of Retribution", C, 41, "buff", 80, 3.0, 0, 60, "friendly", "none",
                  [{"type": "buff", "stat": "damage_shield", "value": 10, "duration": 600}], "Holy light burns whoever strikes your target: 10 damage per hit for 10 minutes."),
            spell("prayer_of_courage", "Prayer of Courage", C, 55, "buff", 150, 3.0, 0, 60, "friendly", "none",
                  [{"type": "buff", "stat": "str", "value": 25, "duration": 1800}, {"type": "buff", "stat": "sta", "value": 25, "duration": 1800}],
                  "+25 strength and stamina for 30 minutes.")]


def wizard_extras() -> list[dict]:
    return [spell("attune_the_weave", "Attune the Weave", "wizard", 3, "buff", 10, 3.0, 0, 0, "self", "none",
                  [{"type": "buff", "stat": "mana_regen", "value": 1, "duration": 1800}], "Tune yourself to the weave: +1 mana regeneration per second for 30 minutes."),
            spell("arcane_barrier", "Arcane Barrier", "wizard", 47, "buff", 90, 3.0, 0, 0, "self", "none",
                  [{"type": "buff", "stat": "damage_taken_pct", "value": -20, "duration": 600}], "A barrier of force: take 20% less melee damage for 10 minutes."),
            spell("veil_of_the_unseen", "Veil of the Unseen", "wizard", 43, "buff", 40, 3.0, 0, 60, "friendly", "none",
                  [{"type": "buff", "stat": "invisible", "value": 1, "duration": 600}], "Bend light around your target: unseen for 10 minutes, until it acts."),
            spell("spirewalk_cragfoot", "Spirewalk: Cragfoot", "wizard", 12, "utility", 50, 6.0, 60, 0, "group", "none",
                  [{"type": "teleport", "destination": "spire_cragfoot"}], "Fold the weave to the crystal spire in the Cragfoot Hills."),
            spell("spirewalk_redrock", "Spirewalk: Redrock", "wizard", 22, "utility", 50, 6.0, 60, 0, "group", "none",
                  [{"type": "teleport", "destination": "spire_redrock"}], "Fold the weave to the crystal spire in the Redrock Badlands."),
            spell("spirewalk_steppe", "Spirewalk: Steppe", "wizard", 32, "utility", 50, 6.0, 60, 0, "group", "none",
                  [{"type": "teleport", "destination": "spire_steppe"}], "Fold the weave to the crystal spire in the Shattered Steppe."),
            spell("spirewalk_stormcrown", "Spirewalk: Stormcrown", "wizard", 50, "utility", 50, 6.0, 60, 0, "group", "none",
                  [{"type": "teleport", "destination": "spire_stormcrown"}], "Fold the weave to the crystal spire in the Stormcrown Peaks.")]


# ------------------------------------------------------------------------------------------- pets and items

# Pet damage is normalised per second: damage_mult / attack_delay sits near an even mob's 1.0 / 2.5,
# below it for the sturdy earth servant and above it only slightly for the fragile fire servant.
PETS = [
    {"id": "ember_servant", "name": "ember servant", "variant": "elemental", "model": "imp", "body": "elemental", "scale": 1.0, "hp_mult": 0.85, "damage_mult": 0.85,
     "attack_delay": 2.2, "run_speed": 7.5, "damage_shield_per_level": 0.5, "resists": {"fire": 200}, "description": "A darting creature of flame that burns whoever strikes it."},
    {"id": "tide_servant", "name": "tide servant", "variant": "elemental", "model": "wraith", "body": "elemental", "scale": 1.0, "hp_mult": 1.05, "damage_mult": 0.85,
     "attack_delay": 2.4, "run_speed": 7.5, "resists": {"cold": 200}, "description": "A steady servant of living water."},
    {"id": "stone_servant", "name": "stone servant", "variant": "elemental", "model": "golem", "body": "construct", "scale": 0.9, "hp_mult": 1.35, "damage_mult": 0.7,
     "attack_delay": 2.8, "run_speed": 6.5, "threat_mult": 1.6, "description": "A slow, enduring guardian of earth that holds a foe's attention."},
    {"id": "gale_servant", "name": "gale servant", "variant": "elemental", "model": "bird", "body": "elemental", "scale": 1.2, "hp_mult": 0.8, "damage_mult": 0.55,
     "attack_delay": 1.6, "run_speed": 9.0, "description": "A whirling servant of air that strikes fast and often."},
    {"id": "bone_thrall", "name": "bone thrall", "variant": "undead", "model": "skeleton", "body": "undead", "scale": 1.0, "hp_mult": 1.05, "damage_mult": 0.85,
     "attack_delay": 2.5, "run_speed": 7.0, "resists": {"disease": 150, "poison": 150}, "description": "Dead bones that rise to the necromancer's call."},
    {"id": "bone_squire", "name": "bone squire", "variant": "undead", "model": "skeleton", "body": "undead", "scale": 0.85, "hp_mult": 0.85, "damage_mult": 0.75,
     "attack_delay": 2.6, "run_speed": 7.0, "resists": {"disease": 100}, "description": "A lesser skeletal servant of a shadow knight."},
    {"id": "spirit_wolf", "name": "spirit wolf", "variant": "spirit", "model": "wolf", "body": "animal", "scale": 1.15, "hp_mult": 0.95, "damage_mult": 0.75,
     "attack_delay": 2.0, "run_speed": 8.5, "description": "A ghostly wolf of the ancestors."},
    {"id": "animated_armor", "name": "animated armor", "variant": "animated", "model": "hollow_knight", "body": "construct", "scale": 0.95, "hp_mult": 0.9, "damage_mult": 0.8,
     "attack_delay": 2.4, "run_speed": 7.0, "description": "An empty suit of armor moved by the enchanter's will."},
]

SUMMONED_ITEMS = [
    {"id": "conjured_bread", "name": "Conjured Bread", "kind": "consumable", "stack": 20, "value": 0, "rarity": "common", "summoned": True,
     "click": {"effect": "fx_conjured_bread", "cast_time": 0, "recast": 0},
     "description": "Warm bread made of magic. Eating it speeds your health regeneration. It fades after an hour."},
    {"id": "conjured_water", "name": "Conjured Water", "kind": "consumable", "stack": 20, "value": 0, "rarity": "common", "summoned": True,
     "click": {"effect": "fx_conjured_water", "cast_time": 0, "recast": 0},
     "description": "A flask of cool, magical water. Drinking it speeds your mana regeneration. It fades after an hour."},
    {"id": "conjured_blade", "name": "Conjured Flameblade", "kind": "weapon", "stack": 1, "value": 0, "rarity": "uncommon", "summoned": True,
     "visual": "sword_basic", "slots": ["primary", "secondary"], "weapon": "1h_slash", "damage": 22, "delay": 2.8, "magic": True, "no_trade": True,
     "proc": {"effect": "fx_conjured_flame", "rate": 0.08},
     "description": "A blade of solid flame that sometimes scorches what it strikes. It fades after thirty minutes."},
]

TRAINERS = [
    # id, name, model, class, greeting
    ("sister_maelis", "Sister Maelis", "trainer_cleric", "cleric", "Light keep you, child. Every prayer I know is written in these scrolls; choose what your faith can bear."),
    ("archmage_torvan", "Archmage Torvan Quill", "trainer_wizard", "wizard", "Ah, an apprentice of the weave. Fire, frost, or pure force? Mind the cost, it is always paid."),
    ("ser_aldric", "Ser Aldric Dawnward", "hollow_knight", "paladin", "Faith is a shield first and a sword second. Learn both."),
    ("lady_vexa", "Lady Vexa Morrow", "hollow_knight", "shadow_knight", "Fear is a tool. So is pain. So is death, if you are clever."),
    ("warden_ilsa", "Warden Ilsa Fernstep", "brigand_archer", "ranger", "The wild teaches patience. These scrolls teach the rest."),
    ("maestro_pell", "Maestro Pell Tuneweaver", "humanoid", "bard", "Every song is a door. Which would you open?"),
    ("elder_brannoch", "Elder Brannoch Oakroot", "treant", "druid", "Root, rain and sun: the grove's gifts, written down for you."),
    ("seer_ruuka", "Seer Ruuka Ashtongue", "humanoid", "shaman", "The spirits speak; I only write down what they say."),
    ("magus_orrin", "Magus Orrin Brightcoil", "cultist", "magician", "Fire, water, earth and air. Name them, and they come."),
    ("mortician_sable", "Mortician Sable Grave", "cultist", "necromancer", "Death is not an end. It is an opportunity."),
    ("mesmer_liora", "Mesmer Liora Veil", "humanoid", "enchanter", "Look into my eyes. Good. Now, which of these scrolls do you want?"),
]

LOCKBOX = {"id": "rusted_strongbox", "name": "a rusted strongbox", "role": "lockbox", "model": "strongbox", "level": 1, "zone": ZONE, "x": 3.0, "z": -10.0,
           "yaw": 0.0, "classes": [], "sells": [], "greeting": "", "lock_level": 15}


# ------------------------------------------------------------------------------------------- checks and output

def check_shape(abilities: list[dict], spells: list[dict]) -> list[str]:
    rules = json.loads((CONTENT / "rules.json").read_text(encoding="utf-8")).get("content", {})
    need, gap = rules.get("min_class_abilities", 0), rules.get("max_level_gap", 0)
    classes = [c["id"] for c in json.loads((CONTENT / "classes.json").read_text(encoding="utf-8"))["classes"]]
    problems = []
    rows = abilities + spells
    ids = [r["id"] for r in rows]
    for dup in sorted({i for i in ids if ids.count(i) > 1}):
        problems.append(f"duplicate id {dup}")
    for r in rows:
        if len(r["id"]) > 32:
            problems.append(f"id too long: {r['id']}")
        if len(r.get("description", "")) > 256:
            problems.append(f"description too long: {r['id']}")
    for c in classes:
        levels = sorted({r["level"] for r in rows if r["class"] == c})
        count = sum(1 for r in rows if r["class"] == c)
        if count < need:
            problems.append(f"{c}: {count} abilities and spells (needs {need})")
        if gap:
            prev = 1
            if 1 not in levels:
                problems.append(f"{c}: nothing at level 1")
            for l in levels:
                if l - prev > gap:
                    problems.append(f"{c}: gap {prev}->{l}")
                prev = l
            if MAX_LEVEL - prev > gap:
                problems.append(f"{c}: nothing after {prev}")
    return problems


def render(comment: str, key: str, rows: list[dict]) -> str:
    lines = ["{", '  "_comment": ' + json.dumps(comment) + ",", f'  "{key}": [']
    for i, r in enumerate(rows):
        lines.append("    " + json.dumps(r) + ("," if i < len(rows) - 1 else ""))
    lines += ["  ]", "}", ""]
    text = "\n".join(lines)
    json.loads(text)
    return text


def main() -> None:
    abilities: list[dict] = []
    spells: list[dict] = base.wizard() + base.cleric() + cleric_extras() + wizard_extras()
    for fn in (warrior, paladin, shadow_knight, ranger, bard, rogue, monk, druid, shaman, magician, necromancer, enchanter):
        a, s = fn()
        abilities += a
        spells += s
    order = {c["id"]: i for i, c in enumerate(json.loads((CONTENT / "classes.json").read_text(encoding="utf-8"))["classes"])}
    abilities.sort(key=lambda r: (order.get(r["class"], 99), r["level"], r["id"]))
    spells.sort(key=lambda r: (order.get(r["class"], 99), r["level"], r["id"]))
    problems = check_shape(abilities, spells)
    if problems:
        sys.exit("content shape problems:\n  " + "\n  ".join(problems))

    abilities_text = render("Class abilities (generated by tools/gen_classes.py; hand-tune there). type is a display category "
                            "(nuke/heal/dot/hot/buff/debuff/cc/pet/utility/passive/discipline); effects drive the mechanics. cost is paid from "
                            "resource. Passives name the skill they represent; their level matches the class skill unlock. position 'behind' "
                            "means the user must stand behind the target.", "abilities", abilities)
    spells_text = render("Spells and Bard songs (generated by tools/gen_classes.py and gen_spells.py; hand-tune there). Learned from scroll "
                         "items and memorized into gems; spells cost mana, songs cost nothing and pulse while sung. See docs/DECISIONS.md "
                         "for stacking, resists, pets, and songs.", "spells", spells)
    pets_text = render("Pet templates (generated by tools/gen_classes.py). A summon_pet effect names one; the pet is a mob of this template "
                       "at the level its spell gives (rules.json mob_scaling with these multipliers).", "pets", PETS)

    idoc = json.loads((CONTENT / "items.json").read_text(encoding="utf-8"))
    summoned_ids = {i["id"] for i in SUMMONED_ITEMS}
    items = [it for it in idoc["items"] if it.get("kind") != "scroll" and it["id"] not in summoned_ids]
    scrolls = [{"id": "scroll_" + s["id"], "name": ("Song: " if s.get("song") else "Spell: ") + s["name"], "kind": "scroll", "stack": 1,
                "value": base.price(s["level"]), "rarity": "common", "spell": s["id"],
                "description": f"A {s['class'].replace('_', ' ')} {'song' if s.get('song') else 'spell'} scroll (level {s['level']}). "
                               f"Scribe it into your spellbook to learn {s['name']}."} for s in spells]
    items_text = render(idoc.get("_comment", ""), "items", items + SUMMONED_ITEMS + scrolls)

    npcs = []
    for i, (nid, name, model, cls, greet) in enumerate(TRAINERS):
        if i < 2:
            x, z = (6.0, 6.0) if i == 0 else (-6.0, 6.0)
        else:
            ang = (i - 2) / (len(TRAINERS) - 2) * 2 * math.pi
            x, z = round(14 * math.cos(ang), 1), round(14 * math.sin(ang), 1)
        sells = ["scroll_" + s["id"] for s in spells if s["class"] == cls]
        npcs.append({"id": nid, "name": name, "role": "trainer", "model": model, "level": 55, "zone": ZONE, "x": x, "z": z,
                     "yaw": round(math.atan2(x, z), 2), "classes": [cls], "sells": sells, "greeting": greet})
    npcs.append(LOCKBOX)
    # Vendors, bankers and auctioneers belong to tools/gen_items.py: keep them.
    kept = [n for n in json.loads((CONTENT / "npcs.json").read_text(encoding="utf-8"))["npcs"] if n.get("role") not in ("trainer", "lockbox") or n.get("origin") == "world"]
    npcs = ordered(npcs + kept)
    npcs_text = render_npcs(npcs)

    outputs = (("abilities.json", abilities_text), ("spells.json", spells_text), ("pets.json", pets_text), ("items.json", items_text),
               ("npcs.json", npcs_text))
    if "--check" in sys.argv[1:]:
        stale = [name for name, text in outputs if (CONTENT / name).read_text(encoding="utf-8") != text]
        if stale:
            sys.exit("gen_classes: stale generated content (run python tools/gen_classes.py): " + ", ".join(stale))
        print("gen_classes: generated content is up to date")
        return
    for name, text in outputs:
        (CONTENT / name).write_text(text, encoding="utf-8", newline="\n")
    counts = {}
    for r in abilities + spells:
        counts[r["class"]] = counts.get(r["class"], 0) + 1
    print(f"wrote {len(abilities)} abilities, {len(spells)} spells, {len(PETS)} pets, {len(scrolls)} scrolls, {len(npcs)} npcs")
    print("per class:", ", ".join(f"{k} {v}" for k, v in counts.items()))


if __name__ == "__main__":
    main()
