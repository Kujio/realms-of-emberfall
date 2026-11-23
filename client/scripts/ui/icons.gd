## Which icon (res://assets/icons/<name>.svg, tools/gen_icons.py) and tint a spell, ability or item
## shows (Phase 9). Spells and abilities pick by type (songs get the note) and tint by resist school;
## items pick by kind, slot or weapon skill and tint by category, and the caller frames them by rarity
## (GameTheme.RARITY). Anything unknown falls back to a neutral icon, so new content needs no art.
extends RefCounted

const Effects := preload("res://scripts/world/effects.gd")
const DIR: String = "res://assets/icons"

const SPELL_ICON: Dictionary = {
	"nuke": "nuke", "heal": "heal", "hot": "hot", "dot": "dot", "buff": "buff", "debuff": "debuff", "cc": "cc",
	"pet": "pet", "utility": "utility", "passive": "passive", "discipline": "discipline",
}
const WEAPON_ICON: Dictionary = {
	"1h_slash": "sword", "2h_slash": "axe", "1h_pierce": "dagger", "2h_pierce": "spear", "1h_blunt": "mace",
	"2h_blunt": "hammer", "archery": "bow", "hand_to_hand": "fist", "throwing": "throwing",
}
## Item tints by icon: metal grey, leather brown, cloth ivory, wood, gold for jewellery, and so on.
const ITEM_TINT: Dictionary = {
	"sword": "#cfd6de", "axe": "#cfd6de", "dagger": "#cfd6de", "spear": "#cfd6de", "mace": "#b8bec6", "hammer": "#b8bec6",
	"bow": "#c79a62", "staff": "#c79a62", "fist": "#d9b48a", "throwing": "#cfd6de", "arrow": "#c79a62",
	"shield": "#b0b8c4", "ring": "#e8c45a", "neck": "#e8c45a", "potion": "#ff6b6b", "food": "#d9a066", "bandage": "#f0ead8",
	"scroll": "#f0e2c0", "bag": "#b0865a", "gem": "#9fd8ff", "key": "#e8c45a", "coin": "#e8c45a",
}
const ARMOR_TINT: Dictionary = {"cloth": "#e6dcc8", "leather": "#b8875a", "chain": "#c4ccd4", "plate": "#dfe6ee"}

static var _tex: Dictionary = {}


static func texture(name: String) -> Texture2D:
	if not _tex.has(name):
		var path: String = DIR.path_join(name + ".svg")
		_tex[name] = load(path) as Texture2D if ResourceLoader.exists(path) else null
	return _tex[name] as Texture2D


## An icon filling `parent` (inset by `margin`), drawn under the parent's labels; hidden until set.
static func make(parent: Control, margin: float) -> TextureRect:
	var t: TextureRect = TextureRect.new()
	t.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	t.offset_left = margin
	t.offset_top = margin
	t.offset_right = -margin
	t.offset_bottom = -margin
	t.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	t.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	t.mouse_filter = Control.MOUSE_FILTER_IGNORE
	t.visible = false
	parent.add_child(t)
	parent.move_child(t, 0)
	return t


## Shows icon [name, tint] (or hides it for []); `dim` greys it (locked or unusable).
static func show(t: TextureRect, icon: Array, dim: bool = false) -> void:
	if icon.is_empty() or texture(str(icon[0])) == null:
		t.visible = false
		return
	t.texture = texture(str(icon[0]))
	var c: Color = icon[1] as Color
	t.modulate = Color(0.45, 0.45, 0.45) if dim else c
	t.visible = true


## [icon name, tint] for a spell or ability by its name (as combat events and the spellbook show it);
## [] for a name that is neither ("Attack", an empty gem).
static func for_spell(spell_name: String, school: int = -1) -> Array:
	var info: Variant = (Effects.data().get("by_name", {}) as Dictionary).get(spell_name)
	if info == null:
		return ["sword", Color("#e8dcc0")] if spell_name == "Attack" else []
	var icon: String = "utility"
	var sch: int = school
	if info is Dictionary:
		var d: Dictionary = info as Dictionary
		icon = "song" if bool(d.get("song", false)) else str(SPELL_ICON.get(str(d.get("type", "")), "utility"))
		if sch < 0:
			sch = Effects.SCHOOLS.find(str(d.get("school", "none")))
	return [icon, Effects.school_color(maxi(sch, 0)).lightened(0.15)]


## [icon name, tint] for an item's display data (ItemDb).
static func for_item(item: Dictionary) -> Array:
	var kind: String = str(item.get("kind", ""))
	var id: String = str(item.get("id", ""))
	var slots: Array = item.get("slots", []) as Array
	var icon: String = "gem"
	match kind:
		"weapon":
			icon = "staff" if id.contains("staff") else str(WEAPON_ICON.get(str(item.get("weapon", "")), "sword"))
		"shield":
			icon = "shield"
		"armor":
			var slot: String = str(slots[0]) if not slots.is_empty() else "chest"
			icon = {"wrist": "arms", "head": "head", "face": "face", "chest": "chest", "shoulders": "shoulders", "back": "back",
				"arms": "arms", "hands": "hands", "waist": "waist", "legs": "legs", "feet": "feet"}.get(slot, "chest")
			return [icon, Color(str(ARMOR_TINT.get(str(item.get("armor", "")), "#d8d2c4")))]
		"jewelry":
			icon = "neck" if slots.has("neck") else "ring"
		"consumable":
			icon = "bandage" if id.contains("bandage") else ("potion" if id.contains("draught") or id.contains("potion") or id.contains("elixir") or id.contains("tonic") else "food")
		"scroll":
			icon = "scroll"
		"bag":
			icon = "bag"
		"ammo":
			icon = "arrow"
		"quest":
			icon = "key"
		_:
			icon = "gem"
	return [icon, Color(str(ITEM_TINT.get(icon, "#d8d2c4")))]
