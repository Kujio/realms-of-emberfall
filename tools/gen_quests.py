#!/usr/bin/env python3
"""Quest generator: writes content/quests.json and the quest givers in content/npcs.json.

Regular quests are written as short specs: a giver, a level, a zone, and a step (kill a family of mobs,
collect trade loot, gather a quest item that drops only while you need it, deliver a letter, or slay a
named boss). Kill steps resolve to whichever mobs of that family actually spawn in the zone (the camps
come from tools/gen_world.py), so the quests always point at real camps. Every class also has an epic
chain of three quests ending in its epic weapon (defined below; tools/gen_items.py creates the items).

  python3 tools/gen_quests.py          # write
  python3 tools/gen_quests.py --check  # exit 1 if a committed file is stale
Run after gen_world.py (camps) and before gen_items.py --check.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from npc_order import render as render_npcs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"

# ------------------------------------------------------------------------------------------- items (gen_items.py makes these)

# Quest items: (id, name, description). Kind quest: no trade, worth nothing to merchants.
QUEST_ITEMS = [
    ("sealed_letter", "Sealed Letter", "A letter sealed with Emberhold's forge-stamp."),
    ("briar_reply", "Briar-bound Reply", "A reply wrapped in a living briar leaf."),
    ("stone_seal", "Stone-stamped Seal", "Cragmouth's answer, pressed into a disc of slate."),
    ("mirefolk_idol", "Mirefolk Idol", "A crude serpent carved in bog oak."),
    ("lens_shard", "Lighthouse Lens Shard", "A curved shard of the old lighthouse lens."),
    ("frost_essence", "Frost Essence", "A wisp of cold that will not melt."),
    ("cinder_core", "Cinder Core", "A coal that never goes out."),
    ("sealed_writ", "Sealed Writ", "A guildmaster's writ, to be shown only to the one it names."),
    ("ancient_shard", "Ancient Shard", "A fragment of a power older than the realm. It hums in your hand."),
]

# Epic weapons: class -> (id, name, weapon type, delay, slots, visual, proc school, stats emphasis). Level 46, epic, lore, no trade.
EPICS = {
    "warrior": ("epic_warrior", "Oathbreaker, Blade of the Unbowed", "2h_slash", 3.8, ["primary"], "sword_basic", "fire", "melee"),
    "paladin": ("epic_paladin", "Dawnhammer of the Last Vigil", "2h_blunt", 3.9, ["primary"], "mace_basic", "magic", "hybrid"),
    "shadow_knight": ("epic_shadow_knight", "Gravesong, the Hungering Edge", "2h_slash", 3.8, ["primary"], "sword_basic", "disease", "hybrid"),
    "ranger": ("epic_ranger", "Windrunner's Longbow", "archery", 3.2, ["ranged"], "bow_on_back", "", "melee"),
    "bard": ("epic_bard", "Lyre-Blade of the Endless Verse", "1h_slash", 2.6, ["primary", "secondary"], "sword_basic", "magic", "hybrid"),
    "rogue": ("epic_rogue", "Whisper and Woe", "1h_pierce", 2.2, ["primary", "secondary"], "dagger_basic", "poison", "melee"),
    "monk": ("epic_monk", "Fists of the Still Storm", "hand_to_hand", 2.0, ["primary", "secondary"], "", "magic", "melee"),
    "cleric": ("epic_cleric", "Mace of the Undying Dawn", "1h_blunt", 3.0, ["primary"], "mace_basic", "magic", "caster"),
    "druid": ("epic_druid", "Thornheart Scimitar", "1h_slash", 2.8, ["primary"], "sword_basic", "fire", "caster"),
    "shaman": ("epic_shaman", "Spirit-Spear of the Ancestors", "2h_pierce", 3.6, ["primary"], "staff_basic", "cold", "caster"),
    "wizard": ("epic_wizard", "Staff of the Ninefold Flame", "2h_blunt", 3.6, ["primary"], "staff_basic", "fire", "caster"),
    "magician": ("epic_magician", "Elemental Conduit", "2h_blunt", 3.6, ["primary"], "staff_basic", "fire", "caster"),
    "necromancer": ("epic_necromancer", "Staff of the Pale Throne", "2h_blunt", 3.6, ["primary"], "staff_basic", "disease", "caster"),
    "enchanter": ("epic_enchanter", "Staff of Infinite Reflections", "2h_blunt", 3.6, ["primary"], "staff_basic", "magic", "caster"),
}

# ------------------------------------------------------------------------------------------- quest givers

# (id, name, zone, x, z, yaw, faction, greeting)
GIVERS = [
    ("captain_brack", "Captain Aldous Brack", "emberhold", -28.0, 32.0, 2.4, "emberhold_watch",
     "The Watch always needs strong arms. Ask, and I will tell you where they are needed."),
    ("mother_tansy", "Old Mother Tansy", "emberhold", 28.0, 32.0, -2.4, "emberhold_watch",
     "Oh, a young one! My knees are not what they were. Could you fetch a few things for an old woman?"),
    ("scholar_wenlow", "Scholar Wenlow", "emberhold", 0.0, -40.0, 0.0, "emberhold_watch",
     "Letters, records, the slow work of keeping three cities talking. Have you a strong pair of legs?"),
    ("warden_sael", "Warden-Captain Sael", "thornwood", -20.0, -60.0, 0.5, "thornwood_wardens",
     "The wood is restless. The wardens can always use another blade."),
    ("elder_mirrin", "Elder Mirrin", "thornwood", 22.0, 75.0, 3.1, "thornwood_wardens",
     "Sit, traveller. The Heartwood remembers every errand ever run beneath it."),
    ("ranger_tavish", "Ranger Tavish", "thornwood", -60.0, 60.0, 1.0, "thornwood_wardens",
     "I track what the wardens cannot spare the time to. Care to lend a hand?"),
    ("marshal_durga", "Marshal Durga Ironvein", "cragmouth", -30.0, 60.0, 1.6, "cragmouth_guard",
     "The skulks test our walls every night. I pay well for every one that stops trying."),
    ("prospector_gill", "Prospector Gill", "cragmouth", 30.0, 60.0, -1.6, "cragmouth_guard",
     "Ore, beetles, the odd golem heart. A prospector takes what the hills give."),
    ("brother_hamm", "Brother Hamm", "cragmouth", 55.0, -40.0, -1.6, "cragmouth_guard",
     "The Deep Flame shows me what the dead are doing. I do not like what it shows."),
    ("fenwatcher_orla", "Fenwatcher Orla", "saltmarsh_fens", 8.0, 68.0, 3.1, "",
     "Keep to the boardwalks if you value your boots. And your life."),
    ("keeper_lowell", "Lighthouse Keeper Lowell", "duskmere_shore", 70.0, -232.0, 3.1, "",
     "The light went out the night the Bastion sank. Some nights I still see its windows under the water."),
    ("outrider_vashti", "Outrider Vashti", "shattered_steppe", 8.0, 8.0, 3.1, "",
     "The cult is stirring on the steppe, and the drakes are hungry. Pick one."),
    ("ashwarden_kell", "Ashwarden Kell", "ashen_reach", -192.0, 158.0, 3.1, "",
     "Few come this far who are not looking for a fight. You will find one."),
]
# Epic guildmasters: class -> (npc id, name, hub, x, z)
EPIC_GIVERS = {
    "warrior": ("epic_warlord_hagen", "Warlord Hagen of the Unbowed", "emberhold", -70.0, 25.0),
    "paladin": ("epic_vigilant_seraphine", "Vigilant Seraphine", "emberhold", 45.0, -30.0),
    "shadow_knight": ("epic_lord_mordrec", "Lord Mordrec the Hollow", "emberhold", -75.0, -60.0),
    "monk": ("epic_master_kaito", "Master Kaito of the Still Hand", "emberhold", 70.0, 25.0),
    "rogue": ("epic_shade_lissel", "Lissel, Who Is Not There", "emberhold", -40.0, 100.0),
    "ranger": ("epic_huntress_aelra", "Huntress Aelra Swiftwind", "thornwood", -75.0, -30.0),
    "bard": ("epic_songmaster_orin", "Songmaster Orin Lightvoice", "thornwood", 75.0, 15.0),
    "druid": ("epic_archdruid_feyn", "Archdruid Feyn Oldroot", "thornwood", 15.0, -40.0),
    "enchanter": ("epic_mirror_sage_ilvae", "Mirror-Sage Ilvae", "thornwood", -30.0, 80.0),
    "magician": ("epic_conjurer_pyrrhus", "Grand Conjurer Pyrrhus", "thornwood", 30.0, 85.0),
    "cleric": ("epic_hierarch_tolvar", "Hierarch Tolvar of the Deep Flame", "cragmouth", 50.0, -45.0),
    "shaman": ("epic_spirit_mother_ruuk", "Spirit-Mother Ruuk", "cragmouth", -85.0, 10.0),
    "wizard": ("epic_archmagus_quill", "Archmagus Zenobia Quill", "cragmouth", 85.0, 10.0),
    "necromancer": ("epic_pale_master_voss", "The Pale Master Voss", "cragmouth", -75.0, -65.0),
}
HUB_FACTION = {"emberhold": "emberhold_watch", "thornwood": "thornwood_wardens", "cragmouth": "cragmouth_guard"}

# Each class's trials: (the outpost contact for the writ, the dungeon whose depths hold the shards, the boss to slay).
EPIC_TRIALS = {
    "warrior": ("ashwarden_kell", "deathless_sanctum", "high_priest_vaurn"),
    "paladin": ("ashwarden_kell", "deathless_sanctum", "high_priest_vaurn"),
    "shadow_knight": ("ashwarden_kell", "hollowmere_crypts", "the_root_mother"),
    "monk": ("outrider_vashti", "stormspire_ruins", "the_root_mother"),
    "rogue": ("outrider_vashti", "stormspire_ruins", "high_priest_vaurn"),
    "ranger": ("outrider_vashti", "gloomroot_caverns", "the_storm_herald"),
    "bard": ("keeper_lowell", "gloomroot_caverns", "the_storm_herald"),
    "druid": ("keeper_lowell", "gloomroot_caverns", "the_root_mother"),
    "enchanter": ("keeper_lowell", "stormspire_ruins", "high_priest_vaurn"),
    "magician": ("outrider_vashti", "stormspire_ruins", "the_storm_herald"),
    "cleric": ("ashwarden_kell", "deathless_sanctum", "the_root_mother"),
    "shaman": ("ashwarden_kell", "gloomroot_caverns", "high_priest_vaurn"),
    "wizard": ("outrider_vashti", "stormspire_ruins", "the_root_mother"),
    "necromancer": ("ashwarden_kell", "deathless_sanctum", "the_storm_herald"),
}

# ------------------------------------------------------------------------------------------- regular quests
# kind: kill (family list, count), collect (trade item, count), gather (quest item, families that drop it, count),
#       deliver (item given, npc to reach), boss (mob id). Rewards: xp in levels, coin, faction.
Q = []


def quest(qid, name, giver, level, zone, kind, arg, count=1, xp=0.35, coin=None, faction=None, prereqs=None, items=None,
          offer="", done="", step_text="", say="", min_level=None):
    Q.append(dict(id=qid, name=name, giver=giver, level=level, zone=zone, kind=kind, arg=arg, count=count, xp=xp,
                  coin=coin if coin is not None else round(5 + level * level * 0.8), faction=faction or {}, prereqs=prereqs or [],
                  items=items or [], offer=offer, done=done, step_text=step_text, say=say, min_level=min_level))


# 1-15
quest("emberglade_vermin", "Vermin in the Glade", "captain_brack", 2, "emberglade", "kill", ["rat", "beetle"], 8, faction={"emberhold_watch": 20},
      offer="Rats and beetles are eating the farmers out of the Emberglade, east of the gate. Thin them out: eight should do it.",
      done="Good work. The farmers will sleep easier.")
quest("emberglade_skulks", "Skulks on the Road", "captain_brack", 5, "emberglade", "kill", ["skulk", "brigand"], 6, faction={"emberhold_watch": 30},
      offer="Skulk raiders have been ambushing carts on the glade road. Put six of them down.", done="That will teach them to rob the Emberhold road.")
quest("tansys_carapaces", "Beetle Shells for Tansy", "mother_tansy", 4, "emberglade", "collect", "beetle_carapace", 4,
      offer="Bring me four beetle carapaces from the Emberglade, dearie. I grind them for my knees.", done="Lovely, lovely. My knees thank you.")
quest("tansys_whiskers", "Whiskers and Wisdom", "mother_tansy", 3, "emberglade", "collect", "rat_whisker", 5,
      offer="Five rat whiskers, if you please. Do not ask what for.", done="Perfect. Now, do not ask.")
quest("letter_to_thornwood", "A Letter to Thornwood", "scholar_wenlow", 3, "thornwood", "deliver", ("sealed_letter", "elder_mirrin"),
      offer="Carry this letter to Elder Mirrin in Thornwood. The waystone keeper can send you, or walk if you have the legs.",
      say="Ah, Wenlow's hand. I shall write back at once.", done="Mirrin's reply is on its way? Splendid.", step_text="Deliver the letter to Elder Mirrin in Thornwood")
quest("reply_from_thornwood", "The Briar-bound Reply", "elder_mirrin", 4, "cragmouth", "deliver", ("briar_reply", "marshal_durga"),
      prereqs=["letter_to_thornwood"], offer="Take my reply on to Marshal Durga in Cragmouth. She must hear it too.",
      say="Thornwood's word, at last. Here: Cragmouth's seal, for Wenlow.", done="", step_text="Carry Mirrin's reply to Marshal Durga in Cragmouth")
quest("the_three_seals", "The Three Seals", "marshal_durga", 5, "emberhold", "deliver", ("stone_seal", "scholar_wenlow"),
      prereqs=["reply_from_thornwood"], offer="Bring Cragmouth's seal back to Scholar Wenlow in Emberhold. Three cities, one accord.",
      say="All three seals! The accord is renewed. You have done the realm a quiet service.", done="", step_text="Bring the stone seal to Scholar Wenlow",
      xp=0.8, faction={"emberhold_watch": 40, "thornwood_wardens": 40, "cragmouth_guard": 40})
quest("warren_bones", "Bones Beneath the Glade", "captain_brack", 8, "rattlebone_warren", "kill", ["skeleton"], 8, faction={"emberhold_watch": 30},
      offer="Something is digging up the old battlefield. The Rattlebone Warren, in the east of the glade. Break eight of the dead.",
      done="Eight fewer. But something commands them.")
quest("the_rattle_king", "The Rattle King", "captain_brack", 11, "rattlebone_warren", "boss", "the_rattle_king", prereqs=["warren_bones"], xp=0.7,
      faction={"emberhold_watch": 60}, items=[("leather_t2_chest", 1)],
      offer="The dead have a king, they say: the Rattle King, in the deepest pit of the warren. End him.", done="The Rattle King, broken at last. Take this, with the Watch's thanks.")
quest("briarwatch_wolves", "Wolves of Briarwatch", "warden_sael", 4, "briarwatch_wood", "kill", ["wolf", "skulk"], 8, faction={"thornwood_wardens": 25},
      offer="Wolves and worse are running bold in Briarwatch Wood, north of our gate. Cull eight.", done="The wood is quieter. Thank you.")
quest("briarwatch_brigands", "Thieves in the Wood", "warden_sael", 6, "briarwatch_wood", "kill", ["brigand"], 6, faction={"thornwood_wardens": 30},
      offer="Brigands have made camp in Briarwatch. The wood will not have them. Nor will I.", done="Well struck.")
quest("tavish_trinkets", "Stolen Trinkets", "ranger_tavish", 7, "briarwatch_wood", "collect", "stolen_trinket", 4,
      offer="The brigands rob pilgrims of their charms. Bring back four of them and I will see them returned.", done="Every one of these is someone's grandmother's. Bless you.")
quest("cragfoot_skulks", "The Skulk Problem", "marshal_durga", 5, "cragfoot_hills", "kill", ["skulk"], 10, faction={"cragmouth_guard": 30},
      offer="Ten skulks, from the Cragfoot Hills below the gate. Bring me ten fewer skulks.", done="Ten fewer skulks. A fine evening's work.")
quest("gills_shells", "Gill's Shell Collection", "prospector_gill", 3, "cragfoot_hills", "collect", "skulk_charm", 3,
      offer="Skulks carry little charms. I collect them. Three, and I will make it worth your while.", done="Hah! A fine set.")
quest("fen_mirefolk", "Scales in the Fen", "fenwatcher_orla", 12, "saltmarsh_fens", "kill", ["mirefolk", "brigand"], 8, faction={"thornwood_wardens": 25},
      offer="Mirefolk are crossing the fens at night, raiding the boardwalk camps. Drive eight of them back into the water.", done="They will think twice.")
quest("fen_idols", "The Serpent Idols", "fenwatcher_orla", 13, "saltmarsh_fens", "gather", ("mirefolk_idol", ["mirefolk"]), 3,
      offer="The mirefolk carry idols of their serpent god. Bring me three; I want to know what they worship.", done="So it is true. They worship something in the Hollow.")
quest("the_mirefang", "Mirefang", "fenwatcher_orla", 17, "mirefang_hollow", "boss", "mirefang", prereqs=["fen_idols"], xp=0.7, faction={"thornwood_wardens": 60},
      offer="Their god is real: a serpent grown huge in the drowned cave they call Mirefang Hollow, in the west of the fens. Kill it.",
      done="The Mirefang is dead? Then the fens may be ours again.", items=[("chain_t3_chest", 1)])
quest("fen_birds", "Herons of Ill Omen", "fenwatcher_orla", 10, "saltmarsh_fens", "kill", ["bird", "rat"], 8,
      offer="Herons and crows pick at the dead in the fens and carry the sickness. Eight of them, please.", done="Thank you. The camp cook is relieved.")
# 15-30
quest("shore_serpents", "Serpents of the Shore", "keeper_lowell", 18, "duskmere_shore", "kill", ["snake", "mirefolk"], 10,
      offer="The shore crawls with serpents since the Bastion sank. Ten of them, and the fishers can work again.", done="The fishers send their thanks, and a little coin.")
quest("lens_shards", "The Broken Lens", "keeper_lowell", 19, "duskmere_shore", "gather", ("lens_shard", ["snake", "mirefolk", "brigand"]), 4,
      offer="The great lens shattered when the light failed. The shore folk have picked over the shards; take four back from them.", done="Four. It is a start. One day the light will shine again.")
quest("the_drowned_admiral", "The Drowned Admiral", "keeper_lowell", 24, "sunken_bastion", "boss", "admiral_grell", prereqs=["lens_shards"], xp=0.7,
      offer="Admiral Grell went down with the Bastion, and he still keeps its watch. The fort lies in the east of the shore. Give him rest.",
      done="Rest, old admiral. Thank you, friend.", items=[("plate_t4_chest", 1)])
quest("redrock_ogres", "The Ogre Clans", "keeper_lowell", 23, "redrock_badlands", "kill", ["giant", "golem"], 8,
      offer="The Redrock ogres are pushing toward the shore. East of here, in the badlands. Push back: eight of them.", done="The badlands will be quieter.")
quest("the_cinder_foreman", "The Cinder Foreman", "keeper_lowell", 27, "cinderdeep_mines", "boss", "the_cinder_foreman", prereqs=["redrock_ogres"], xp=0.7,
      offer="The ogres work slaves in the Cinderdeep Mines, in the badlands' far corner, under a brute they call the Foreman. Break his whip.",
      done="Free, all of them. You did a good thing.", items=[("leather_t4_chest", 1)])
quest("hollowpine_treants", "The Waking Trees", "elder_mirrin", 23, "hollowpine_forest", "kill", ["treant", "spider"], 8, faction={"thornwood_wardens": 40},
      offer="The trees of Hollowpine are waking angry, and spiders nest in them. West of the Duskmere. Calm eight, any way you can.", done="The forest sighs. Thank you.")
quest("gloomfen_horrors", "Horrors of the Gloomfen", "brother_hamm", 26, "gloomfen_bog", "kill", ["horror", "treant", "spider"], 8, faction={"cragmouth_guard": 40},
      offer="North of Hollowpine lies the Gloomfen, where horrors breed in the black water. Eight fewer, and the Flame will be pleased.", done="The Flame is pleased.")
quest("the_pale_abbot", "The Pale Abbot", "brother_hamm", 33, "hollowmere_crypts", "boss", "the_pale_abbot", prereqs=["gloomfen_horrors"], xp=0.8,
      faction={"cragmouth_guard": 60}, offer="Beneath the Gloomfen sleeps the abbey of Hollowmere, and its abbot does not sleep at all. End his vigil.",
      done="The abbot rests. The Flame thanks you, and so do I.", items=[("plate_t5_chest", 1)])
# 30-45
quest("steppe_cultists", "Ash on the Wind", "outrider_vashti", 33, "shattered_steppe", "kill", ["cultist", "knight"], 10,
      faction={"emberhold_watch": 30, "thornwood_wardens": 30, "cragmouth_guard": 30},
      offer="The Ashen Cult is gathering on the steppe. Ten of their faithful, and their rites will falter.", done="Their fires are fewer tonight.")
quest("steppe_wolves", "The Plains Hunters", "outrider_vashti", 31, "shattered_steppe", "kill", ["wolf", "bird"], 10,
      offer="The plains stalkers are taking our horses. Ten of them.", done="The herd is safe. For now.")
quest("the_storm_herald", "The Storm Herald", "outrider_vashti", 41, "stormspire_ruins", "boss", "the_storm_herald", prereqs=["steppe_cultists"], xp=0.8,
      offer="Their leader calls the lightning from the Stormspire Ruins, in the steppe's far corner. Silence the Storm Herald.",
      done="The storm breaks. Well done.", items=[("chain_t6_chest", 1)])
quest("frost_essences", "Essence of Frost", "outrider_vashti", 39, "frostveil_tundra", "gather", ("frost_essence", ["wraith", "wolf", "giant"]), 4,
      offer="North of the steppe, the Frostveil's creatures carry a cold that never melts. Bring me four essences; the alchemists pay well.", done="Cold as the grave. Perfect.")
quest("the_root_mother", "The Root Mother", "outrider_vashti", 46, "gloomroot_caverns", "boss", "the_root_mother", prereqs=["frost_essences"], xp=0.8,
      offer="Beneath the Frostveil, warm caves; in them, roots; at their heart, something that feeds. Kill the Root Mother.",
      done="The roots wither. The tundra will freeze clean again.", items=[("leather_t6_chest", 1)])
quest("cinder_cores", "Cores of the Cinderspire", "outrider_vashti", 41, "cinderspire_wastes", "gather", ("cinder_core", ["imp", "beetle", "cultist"]), 4,
      offer="East of the steppe, the Cinderspire's creatures burn from within. Four of their cores, and our forges will never go cold.", done="Still burning. Wonderful.")
quest("cinder_zealots", "The Burning Faithful", "outrider_vashti", 42, "cinderspire_wastes", "kill", ["cultist", "drake"], 10,
      offer="The cult's zealots keep their fires in the Cinderspire Wastes. Ten of them.", done="The wastes burn a little less.")
# 45-60
quest("ashen_trolls", "Trolls of the Ash", "ashwarden_kell", 48, "ashen_reach", "kill", ["giant", "drake"], 10,
      offer="Trolls and wyrmlings hunt the ash between here and the Stormcrown. Ten of them.", done="Ten fewer. The reach thanks you.")
quest("ashen_dead", "The Deathless Vigil", "ashwarden_kell", 49, "ashen_reach", "kill", ["skeleton", "knight", "wraith", "cultist"], 10,
      faction={"emberhold_watch": 30, "thornwood_wardens": 30, "cragmouth_guard": 30},
      offer="The deathless keep their vigil across the reach. Break ten of them.", done="Ten more at rest.")
quest("high_priest_vaurn", "High Priest Vaurn", "ashwarden_kell", 53, "deathless_sanctum", "boss", "high_priest_vaurn", prereqs=["ashen_dead"], xp=0.8,
      offer="The Deathless Sanctum lies in the east of the reach. Its high priest would make the whole realm deathless. Stop him.",
      done="Vaurn is dead. Truly dead. I did not think it could be done.", items=[("plate_t7_chest", 1)])
quest("the_obsidian_tyrant", "The Obsidian Tyrant", "ashwarden_kell", 58, "obsidian_citadel", "boss", "the_obsidian_tyrant", prereqs=["high_priest_vaurn"],
      xp=1.0, offer="The Obsidian Citadel, in the west of the reach: a fortress of black glass, and a tyrant who rules it. Gather a raid. Break him.",
      done="The citadel is shattered. The realm will sing of this.", items=[("chain_t8_chest", 1)])
quest("stormcrown_drakes", "Drakes Above the Clouds", "ashwarden_kell", 56, "stormcrown_peaks", "kill", ["drake", "golem", "bird"], 10,
      offer="North of the reach, the Stormcrown. The Emberwyrm's children nest on its peaks. Ten of them.", done="Ten. The mother will notice.")
quest("the_emberwyrm", "The Emberwyrm", "ashwarden_kell", 60, "emberwyrm_lair", "boss", "the_emberwyrm", prereqs=["stormcrown_drakes"], xp=1.0,
      faction={"emberhold_watch": 100, "thornwood_wardens": 100, "cragmouth_guard": 100},
      offer="She sleeps in the burning heart of the Stormcrown, on a hoard a thousand years old. The Emberwyrm. Gather everyone you trust.",
      done="The Emberwyrm is slain. The fires that scarred the reach are cold at last. You are a legend.", items=[("plate_t8_chest", 1)])


def family_mobs(zones: dict, mobs: dict, zone_id: str, families: list[str]) -> list[str]:
    """Mob ids of these families that spawn in the zone (dungeons: every mob of theirs)."""
    z = zones[zone_id]
    ids = set()
    for s in z.get("spawns", []):
        for mid in (s["mob"], s.get("rare")):
            m = mobs.get(mid or "")
            if m and m.get("family") in families and not m.get("boss"):
                ids.add(m["id"])
    return sorted(ids)


def build(zones: dict, mobs: dict) -> list[dict]:
    out = []
    for q in Q:
        qd = {"id": q["id"], "name": q["name"], "giver": q["giver"], "level": q["level"]}
        if q["min_level"]:
            qd["min_level"] = q["min_level"]
        if q["prereqs"]:
            qd["prereqs"] = q["prereqs"]
        qd["zone"] = q["zone"]
        qd["offer"] = q["offer"]
        qd["accepted"] = "Off with you, then."
        qd["progress"] = "Not yet finished? There is no shame in taking your time. There is shame in giving up."
        qd["complete"] = q["done"] or "Well done."
        kind, arg, count = q["kind"], q["arg"], q["count"]
        zname = zones[q["zone"]]["name"]
        if kind == "kill":
            ids = family_mobs(zones, mobs, q["zone"], arg)
            if not ids:
                sys.exit(f"gen_quests: {q['id']}: no {arg} spawn in {q['zone']}")
            qd["steps"] = [{"type": "kill", "mobs": ids, "count": count, "text": q["step_text"] or f"Slay {count} in {zname}"}]
        elif kind == "collect":
            qd["steps"] = [{"type": "collect", "item": arg, "count": count, "text": q["step_text"] or f"Collect {count} {arg.replace('_', ' ')}s"}]
        elif kind == "gather":
            item, fams = arg
            ids = family_mobs(zones, mobs, q["zone"], fams)
            if not ids:
                sys.exit(f"gen_quests: {q['id']}: nothing in {q['zone']} drops {item}")
            name = next(n for i, n, _ in QUEST_ITEMS if i == item)
            qd["steps"] = [{"type": "collect", "item": item, "count": count, "drops_from": ids, "chance": 0.35,
                            "text": q["step_text"] or f"Gather {count} {name}s in {zname}"}]
        elif kind == "deliver":
            item, npc = arg
            qd["give"] = [{"item": item, "count": 1}]
            qd["steps"] = [{"type": "deliver", "npc": npc, "item": item, "text": q["step_text"], "say": q["say"]}]
            qd["turn_in"] = npc
            qd["complete"] = q["done"] or q["say"] or "Thank you."
        elif kind == "boss":
            boss = mobs[arg]
            qd["steps"] = [{"type": "kill", "mobs": [arg], "count": 1, "text": f"Slay {boss['name']} in {zones[q['zone']]['name']}"}]
        rewards = {"xp": q["xp"], "coin": q["coin"]}
        if q["items"]:
            rewards["items"] = [{"item": i, "count": n} for i, n in q["items"]]
        if q["faction"]:
            rewards["faction"] = q["faction"]
        qd["rewards"] = rewards
        out.append(qd)
    # Epic chains: the summons (a writ to deliver), the trial (shards from a dungeon's depths), the reckoning (its boss).
    classes = json.loads((CONTENT / "classes.json").read_text(encoding="utf-8"))["classes"]
    for cl in classes:
        cid = cl["id"]
        gid, gname, hub, _, _ = EPIC_GIVERS[cid]
        contact, dungeon, boss = EPIC_TRIALS[cid]
        wid, wname = EPICS[cid][0], EPICS[cid][1]
        contact_name = next(g[1] for g in GIVERS if g[0] == contact)
        dz = zones[dungeon]
        depths = sorted(m["id"] for m in mobs.values() if m.get("dungeon") == dungeon and not m.get("boss"))
        bname = mobs[boss]["name"]
        cname = cl["name"]
        base = {"giver": gid, "classes": [cid], "epic": True}
        out.append({"id": f"epic_{cid}_1", "name": f"{cname}'s Calling", "level": 44, "min_level": 44, **base, "zone": zones[next(g[2] for g in GIVERS if g[0] == contact)]["id"],
                    "offer": f"You have grown strong, {cname}. Strong enough, perhaps, for what our order keeps. Carry this writ to {contact_name}; "
                             f"they will know what it means.",
                    "accepted": "Go. Do not open it.", "progress": "The writ is not delivered.",
                    "complete": f"{contact_name} has read it? Then the trials begin.",
                    "give": [{"item": "sealed_writ", "count": 1}],
                    "steps": [{"type": "deliver", "npc": contact, "item": "sealed_writ", "text": f"Bring the writ to {contact_name}",
                               "say": f"A writ from the {cname}s' order... So, they think you are ready. Go back; tell them I agree."},
                              {"type": "talk", "npc": gid, "text": f"Return to {gname}", "say": "Good. Now the real work."}],
                    "rewards": {"xp": 0.3, "coin": 500}})
        out.append({"id": f"epic_{cid}_2", "name": f"The {cname}'s Trial", "level": 47, "min_level": 45, **base, "prereqs": [f"epic_{cid}_1"], "zone": dungeon,
                    "offer": f"In the depths of {dz['name']} lie shards of the power our order's first {cname} wielded. Bring me three.",
                    "accepted": "Three shards. No fewer.", "progress": "Three shards. I can wait.", "complete": "Three. Feel how they hum together?",
                    "steps": [{"type": "collect", "item": "ancient_shard", "count": 3, "drops_from": depths, "chance": 0.15,
                               "text": f"Gather 3 Ancient Shards in {dz['name']}"}],
                    "rewards": {"xp": 0.4, "coin": 1000}})
        out.append({"id": f"epic_{cid}_3", "name": wname, "level": 50, "min_level": 46, **base, "prereqs": [f"epic_{cid}_2"], "zone": next(
                        z for z in zones.values() for s in z.get("spawns", []) if s["mob"] == boss)["id"],
                    "offer": f"The shards will wake only in the presence of a great death. Slay {bname}, and I will forge what is yours by right.",
                    "accepted": "Go, and come back changed.", "progress": f"{bname} still lives.",
                    "complete": f"It is done. Take it: {wname}. Our order's first {cname} would be proud.",
                    "steps": [{"type": "kill", "mobs": [boss], "count": 1, "text": f"Slay {bname}"}],
                    "rewards": {"xp": 0.6, "coin": 2500, "items": [{"item": wid, "count": 1}]}})
    return out


def givers() -> list[dict]:
    out = []
    for gid, name, zone, x, z, yaw, faction, greet in GIVERS:
        row = {"id": gid, "name": name, "role": "questgiver", "model": "humanoid", "level": 50, "zone": zone, "x": x, "z": z, "yaw": yaw, "greeting": greet}
        if faction:
            row["faction"] = faction
        out.append(row)
    for cid, (gid, name, hub, x, z) in EPIC_GIVERS.items():
        out.append({"id": gid, "name": name, "role": "questgiver", "model": "humanoid", "level": 60, "zone": hub, "x": x, "z": z, "yaw": 0.0,
                    "greeting": "Few come to me. Fewer leave unchanged.", "faction": HUB_FACTION[hub]})
    return out


def main() -> int:
    check = "--check" in sys.argv
    zones = {}
    for p in sorted((CONTENT / "zones").glob("*.json")):
        z = json.loads(p.read_text(encoding="utf-8"))
        zones[z["id"]] = z
    mobs = {m["id"]: m for m in json.loads((CONTENT / "mobs.json").read_text(encoding="utf-8"))["mobs"]}
    quests = build(zones, mobs)
    lines = ["{", '  "_comment": ' + json.dumps("Quests (generated by tools/gen_quests.py): regular quests across the bands, a three-city letter chain, "
                                             "dungeon bosses, and a three-part epic chain per class. Steps: kill, collect (counted from the pack; "
                                             "quest items with drops_from fall only while needed), talk/deliver. rewards.xp is in levels.") + ",",
             '  "quests": [']
    lines += ["    " + json.dumps(q) + ("," if i < len(quests) - 1 else "") for i, q in enumerate(quests)]
    lines += ["  ]", "}", ""]
    q_text = "\n".join(lines)
    npc_path = CONTENT / "npcs.json"
    kept = [n for n in json.loads(npc_path.read_text(encoding="utf-8"))["npcs"] if n.get("role") != "questgiver"]
    npc_text = render_npcs(kept + givers())
    outputs = [(CONTENT / "quests.json", q_text), (npc_path, npc_text)]
    stale = []
    for path, text in outputs:
        if check:
            if path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
    if check and stale:
        print("gen_quests: stale generated content (run python3 tools/gen_quests.py):", ", ".join(stale))
        return 1
    if not check:
        epics = sum(1 for q in quests if q.get("epic"))
        print(f"wrote {len(quests)} quests ({len(quests) - epics} regular, {epics} in {epics // 3} epic chains) and {len(givers())} quest givers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
