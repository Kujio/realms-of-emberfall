## The client's copy of item display data (client/data/items.json, synced from content by
## tools/sync_client_content.py), slot codes, and tooltip text with comparison against what is worn.
## The server stays authoritative: this only decides how things look.
extends RefCounted

const DATA_PATH: String = "res://data/items.json"

# Slot codes: (container << 8) | index, the same as the server (game/items.hpp).
const CONT_EQUIP: int = 0x00
const CONT_GENERAL: int = 0x01
const CONT_BANK: int = 0x02
const CONT_GENERAL_BAG: int = 0x10
const CONT_BANK_BAG: int = 0x20
const GENERAL_SLOTS: int = 8
const BANK_SLOTS: int = 16
const NO_SLOT: int = 0xFFFF

const EQUIP_NAMES: Array[String] = ["head", "face", "neck", "shoulders", "back", "chest", "arms", "wrist1", "wrist2", "hands",
	"finger1", "finger2", "waist", "legs", "feet", "primary", "secondary", "ranged", "ammo", "ear1", "ear2"]
const EQUIP_LABELS: Array[String] = ["Head", "Face", "Neck", "Shoulders", "Back", "Chest", "Arms", "Wrist", "Wrist", "Hands",
	"Finger", "Finger", "Waist", "Legs", "Feet", "Primary", "Secondary", "Ranged", "Ammo", "Ear", "Ear"]
const HEAD: int = 0
const CHEST: int = 5
const PRIMARY: int = 15
const SECONDARY: int = 16
const RANGED: int = 17
const RARITY_COLORS: Dictionary = {
	"common": Color(0.9, 0.9, 0.9), "uncommon": Color(0.35, 0.9, 0.4), "rare": Color(0.4, 0.6, 1.0), "epic": Color(0.75, 0.45, 1.0),
}
const STAT_ORDER: Array[String] = ["str", "sta", "agi", "dex", "wis", "int", "cha"]

static var _data: Dictionary = {}


static func data() -> Dictionary:
	if _data.is_empty():
		var f: FileAccess = FileAccess.open(DATA_PATH, FileAccess.READ)
		if f != null:
			var parsed: Variant = JSON.parse_string(f.get_as_text())
			if parsed is Dictionary:
				_data = parsed
		if _data.is_empty():
			push_warning("items: %s is missing or unreadable" % DATA_PATH)
			_data = {"items": {}, "effects": {}}
	return _data


## Replaces the data (client tests).
static func override_data(d: Dictionary) -> void:
	_data = d


static func item(id: String) -> Dictionary:
	var v: Variant = (data().get("items", {}) as Dictionary).get(id)
	return v if v is Dictionary else {}


static func name_of(id: String) -> String:
	var it: Dictionary = item(id)
	return str(it.get("name", id))


static func effect(id: String) -> Dictionary:
	var v: Variant = (data().get("effects", {}) as Dictionary).get(id)
	return v if v is Dictionary else {}


static func code(container: int, index: int) -> int:
	return (container << 8) | index


static func container_of(c: int) -> int:
	return (c >> 8) & 0xFF


static func index_of(c: int) -> int:
	return c & 0xFF


static func is_bag_content(c: int) -> bool:
	var k: int = container_of(c)
	return (k >= CONT_GENERAL_BAG and k < CONT_GENERAL_BAG + GENERAL_SLOTS) or (k >= CONT_BANK_BAG and k < CONT_BANK_BAG + BANK_SLOTS)


static func in_bank(c: int) -> bool:
	var k: int = container_of(c)
	return k == CONT_BANK or (k >= CONT_BANK_BAG and k < CONT_BANK_BAG + BANK_SLOTS)


## The slot holding the bag whose contents `c` is in (NO_SLOT otherwise).
static func bag_parent(c: int) -> int:
	var k: int = container_of(c)
	if k >= CONT_GENERAL_BAG and k < CONT_GENERAL_BAG + GENERAL_SLOTS:
		return code(CONT_GENERAL, k - CONT_GENERAL_BAG)
	if k >= CONT_BANK_BAG and k < CONT_BANK_BAG + BANK_SLOTS:
		return code(CONT_BANK, k - CONT_BANK_BAG)
	return NO_SLOT


## The container holding a bag's contents, for a pack or bank slot.
static func bag_container(parent: int) -> int:
	if container_of(parent) == CONT_GENERAL:
		return CONT_GENERAL_BAG + index_of(parent)
	if container_of(parent) == CONT_BANK:
		return CONT_BANK_BAG + index_of(parent)
	return -1


## Equipment indices an item can be worn in.
static func equip_slots_of(it: Dictionary) -> Array[int]:
	var out: Array[int] = []
	for s: Variant in it.get("slots", []):
		var name: String = str(s)
		match name:
			"wrist":
				out.append_array([7, 8])
			"finger":
				out.append_array([10, 11])
			"ear":
				out.append_array([19, 20])
			_:
				var i: int = EQUIP_NAMES.find(name)
				if i >= 0:
					out.append(i)
	return out


static func fits(it: Dictionary, equip_index: int) -> bool:
	return equip_slots_of(it).has(equip_index)


static func rarity_color(id: String) -> Color:
	return RARITY_COLORS.get(str(item(id).get("rarity", "common")), Color.WHITE) as Color


static func bag_slots(id: String) -> int:
	return int(item(id).get("bag_slots", 0))


static func is_bag(id: String) -> bool:
	return str(item(id).get("kind", "")) == "bag"


## A short label for an item square: initials of its name.
static func short_name(id: String) -> String:
	var words: PackedStringArray = name_of(id).split(" ", false)
	var out: String = ""
	for w: String in words:
		if out.length() >= 3:
			break
		out += w.substr(0, 1)
	return out.to_upper()


static func format_coin(copper: int) -> String:
	var parts: Array[String] = []
	var pp: int = copper / 1000
	var gp: int = (copper / 100) % 10
	var sp: int = (copper / 10) % 10
	var cp: int = copper % 10
	if pp > 0:
		parts.append("%dp" % pp)
	if gp > 0:
		parts.append("%dg" % gp)
	if sp > 0:
		parts.append("%ds" % sp)
	if cp > 0 or parts.is_empty():
		parts.append("%dc" % cp)
	return " ".join(parts)


## Numbers an item adds, keyed for comparison: ac, hp, mana, endurance, stats, resists, haste, dps.
static func numbers(it: Dictionary) -> Dictionary:
	var n: Dictionary = {}
	for k: String in ["ac", "hp", "mana", "endurance", "haste"]:
		if it.has(k) and int(it[k]) != 0:
			n[k] = int(it[k])
	var st: Dictionary = it.get("stats", {}) as Dictionary
	for k: String in STAT_ORDER:
		if st.has(k) and int(st[k]) != 0:
			n[k] = int(st[k])
	var rs: Dictionary = it.get("resists", {}) as Dictionary
	for k: String in rs.keys():
		n["resist_" + k] = int(rs[k])
	if str(it.get("kind", "")) == "weapon" and float(it.get("delay", 0.0)) > 0.0:
		n["dps"] = snappedf(float(it.get("damage", 0)) / float(it["delay"]), 0.01)
	return n


static func _label(k: String) -> String:
	match k:
		"ac": return "AC"
		"hp": return "HP"
		"mana": return "Mana"
		"endurance": return "Endurance"
		"haste": return "Haste %"
		"dps": return "Damage/sec"
	if k.begins_with("resist_"):
		return "Resist " + k.substr(7).capitalize()
	return k.to_upper()


## The lines of a tooltip. `worn` is the item currently in the slot this one would go in ({} = none).
static func tooltip_lines(id: String, count: int, worn: Dictionary = {}) -> Array[String]:
	var it: Dictionary = item(id)
	var lines: Array[String] = []
	if it.is_empty():
		lines.append(id)
		return lines
	var title: String = str(it.get("name", id))
	if count > 1:
		title += " (%d)" % count
	lines.append(title)
	var tags: Array[String] = [str(it.get("rarity", "common")).capitalize()]
	for flag: String in ["magic", "lore", "no_trade", "summoned"]:
		if bool(it.get(flag, false)):
			tags.append(flag.replace("_", "-").to_upper() if flag != "magic" else "MAGIC")
	lines.append(", ".join(tags))
	var kind: String = str(it.get("kind", ""))
	var slots: Array[String] = []
	for s: Variant in it.get("slots", []):
		slots.append(str(s).capitalize())
	if not slots.is_empty():
		var what: String = kind.capitalize()
		if it.has("armor"):
			what = "%s armor" % str(it["armor"]).capitalize()
		elif it.has("weapon"):
			what = str(it["weapon"]).replace("_", " ").replace("1h", "one-handed").replace("2h", "two-handed").capitalize()
		lines.append("%s - %s" % [what, ", ".join(slots)])
	elif kind != "":
		lines.append("Crafting material" if kind == "tradeskill" else kind.capitalize())
	if kind == "weapon" or (kind == "ammo" and int(it.get("damage", 0)) > 0):
		if float(it.get("delay", 0.0)) > 0.0:
			lines.append("Damage %d  Delay %.1fs  (%.1f per second)" % [int(it.get("damage", 0)), float(it["delay"]), float(it.get("damage", 0)) / float(it["delay"])])
		else:
			lines.append("Damage +%d" % int(it.get("damage", 0)))
	var n: Dictionary = numbers(it)
	for k: String in ["ac", "hp", "mana", "endurance"] + STAT_ORDER + ["haste"]:
		if n.has(k):
			lines.append("%s %+d" % [_label(k), int(n[k])])
	for k: String in n.keys():
		if str(k).begins_with("resist_"):
			lines.append("%s %+d" % [_label(k), int(n[k])])
	if it.has("proc"):
		var fx: Dictionary = effect(str((it["proc"] as Dictionary).get("effect", "")))
		lines.append("Proc: %s. %s" % [str(fx.get("name", "?")), str(fx.get("description", ""))])
	if it.has("click"):
		var c: Dictionary = it["click"] as Dictionary
		var fx2: Dictionary = effect(str(c.get("effect", "")))
		var how: String = "Use" if kind == "consumable" else "Click"
		var cast: float = float(c.get("cast_time", 0.0))
		lines.append("%s: %s%s" % [how, str(fx2.get("description", fx2.get("name", ""))), (" (%.0fs to use)" % cast) if cast > 0.0 else ""])
	if int(it.get("bag_slots", 0)) > 0:
		lines.append("Holds %d items" % int(it["bag_slots"]))
	if int(it.get("req_level", 0)) > 1:
		lines.append("Requires level %d" % int(it["req_level"]))
	for key: String in ["classes", "races"]:
		var l: Array = it.get(key, []) as Array
		if not l.is_empty():
			var names: Array[String] = []
			for x: Variant in l:
				names.append(str(x).replace("_", " ").capitalize())
			lines.append("%s: %s" % [key.capitalize(), ", ".join(names)])
	if str(it.get("description", "")) != "":
		lines.append(str(it["description"]))
	if int(it.get("value", 0)) > 0:
		lines.append("Value: %s" % format_coin(int(it["value"])))
	if not worn.is_empty() or not slots.is_empty():
		var diff: Array[String] = compare_lines(it, worn)
		if not diff.is_empty():
			lines.append("Compared to %s:" % (str(worn.get("name", "")) if not worn.is_empty() else "nothing worn"))
			lines.append_array(diff)
	return lines


static func tooltip(id: String, count: int, worn: Dictionary = {}) -> String:
	return "\n".join(tooltip_lines(id, count, worn))


## "+5 AC", "-2 STR" ...: what wearing `it` instead of `worn` changes.
static func compare_lines(it: Dictionary, worn: Dictionary) -> Array[String]:
	var a: Dictionary = numbers(it)
	var b: Dictionary = numbers(worn)
	var keys: Array[String] = []
	for k: Variant in a.keys():
		keys.append(str(k))
	for k: Variant in b.keys():
		if not keys.has(str(k)):
			keys.append(str(k))
	var order: Array[String] = ["dps", "ac", "hp", "mana", "endurance"]
	order.append_array(STAT_ORDER)
	order.append("haste")
	keys.sort_custom(func(x: String, y: String) -> bool:
		var ix: int = order.find(x)
		var iy: int = order.find(y)
		return (ix if ix >= 0 else 99) < (iy if iy >= 0 else 99))
	var out: Array[String] = []
	for k: String in keys:
		var d: float = float(a.get(k, 0)) - float(b.get(k, 0))
		if absf(d) < 0.001:
			continue
		if k == "dps":
			out.append("  %+.1f %s" % [d, _label(k)])
		else:
			out.append("  %+d %s" % [int(d), _label(k)])
	return out


## What is worn where `it` would go: the first fitting slot that is empty wins (nothing to compare),
## otherwise the first fitting worn item. `worn_by_index` maps equipment index -> item id.
static func comparison_item(it: Dictionary, worn_by_index: Dictionary) -> Dictionary:
	var first: Dictionary = {}
	for i: int in equip_slots_of(it):
		if not worn_by_index.has(i):
			return {}
		if first.is_empty():
			first = item(str(worn_by_index[i]))
	return first
