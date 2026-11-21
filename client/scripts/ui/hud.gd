## In-world combat HUD: player frame, target frame (con color), XP bar, 10-slot hotbar with
## cooldowns, ability window (drag to hotbar), inventory, loot window, and the death dialog.
## The spell half (gems, cast bar, buffs, spellbook, trainer, resurrection) lives in `spells`.
## Pure presentation: it emits intents as signals and the world sends them to the server.
extends Control

signal slot_used(index: int)
signal ability_used(ability_id: String)
signal loot_requested
signal loot_take(slot: int)
signal loot_close
signal respawn_requested
signal trainer_requested
signal scribe_requested(slot: int)
## A message for the player (shown in chat).
signal notice(text: String)

const UI := preload("res://scripts/ui/ui.gd")
const HotbarSlot := preload("res://scripts/ui/hotbar_slot.gd")
const AbilityRow := preload("res://scripts/ui/ability_row.gd")
const SpellUi := preload("res://scripts/ui/spell_ui.gd")

const SLOT_COUNT: int = 10
const ATTACK_ID: String = "auto_attack"
const LOOT_ALL: int = 255
const CON_COLORS: Array[Color] = [
	Color(0.6, 0.6, 0.6), Color(0.35, 0.9, 0.35), Color(0.45, 0.65, 1.0),
	Color(1.0, 1.0, 1.0), Color(1.0, 0.9, 0.25), Color(1.0, 0.3, 0.3),
]
const CON_NAMES: Array[String] = ["grey", "green", "blue", "white", "yellow", "red"]
const DEFAULT_BAR: Array[String] = [
	"auto_attack", "rising_kick", "iron_challenge", "pommel_crack", "rallying_cry",
	"cleaving_blow", "bulwark_stance", "provoking_roar", "battle_fury", "stalwart_resolve",
]
## Casters (no abilities, a spellbook): keys 2-9 cast spell gems 1-8.
const CASTER_BAR: Array[String] = ["auto_attack", "gem:0", "gem:1", "gem:2", "gem:3", "gem:4", "gem:5", "gem:6", "gem:7", ""]
const GEM_PREFIX: String = "gem:"
const KIND_NPC: int = 3

var my_name: String = ""
var level: int = 1
var abilities: Dictionary = {}  # id -> AbilityInfo dictionary
var ability_order: Array[String] = []
var hotbar_ids: Array[String] = []
var slots: Array = []  # HotbarSlot nodes
var auto_attack: bool = false
var dead: bool = false
var cooldown_end: Dictionary = {}    # ability id -> msec
var cooldown_total: Dictionary = {}  # ability id -> seconds
var gcd_end: int = 0
var gcd_total: float = 1.5
var loot_corpse: int = 0
var loot_items: Array = []
var inventory_coin: int = 0
var inventory_items: Array = []
var target_kind: int = -1
var spells: Control  # SpellUi

var _hp: ProgressBar
var _end: ProgressBar
var _mana: ProgressBar
var _name_label: Label
var _status: Label
var _target_panel: PanelContainer
var _target_name: Label
var _target_hp: ProgressBar
var _target_loot: Button
var _target_train: Button
var _target_style: StyleBoxFlat
var _xp: ProgressBar
var _xp_label: Label
var _abilities_win: PanelContainer
var _ability_list: VBoxContainer
var _inv_win: PanelContainer
var _inv_list: VBoxContainer
var _inv_coin: Label
var _loot_win: PanelContainer
var _loot_title: Label
var _loot_list: VBoxContainer
var _death: PanelContainer
var _death_text: Label
var _save_path: String = ""


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_player_frame()
	_build_target_frame()
	_build_hotbar()
	_build_ability_window()
	_build_inventory_window()
	_build_loot_window()
	_build_death_dialog()
	spells = SpellUi.new()
	add_child(spells)
	spells.connect("notice", func(t: String) -> void: notice.emit(t))


# ------------------------------------------------------------------------------ construction

func _panel(parent: Control, alpha: float = 0.55) -> PanelContainer:
	var p: PanelContainer = PanelContainer.new()
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = Color(0.05, 0.04, 0.04, alpha)
	s.border_color = Color(0.4, 0.3, 0.22)
	s.set_border_width_all(1)
	s.set_corner_radius_all(4)
	s.set_content_margin_all(8)
	p.add_theme_stylebox_override("panel", s)
	parent.add_child(p)
	return p


## Anchors (left, top, right, bottom) as fractions of the screen, then pixel offsets from them.
static func _anchor(c: Control, anchors: Vector4, offsets: Vector4) -> void:
	c.anchor_left = anchors.x
	c.anchor_top = anchors.y
	c.anchor_right = anchors.z
	c.anchor_bottom = anchors.w
	c.offset_left = offsets.x
	c.offset_top = offsets.y
	c.offset_right = offsets.z
	c.offset_bottom = offsets.w


func _bar(parent: Control, fill: Color, height: float) -> ProgressBar:
	var b: ProgressBar = ProgressBar.new()
	b.custom_minimum_size = Vector2(0, height)
	b.show_percentage = false
	var bg: StyleBoxFlat = StyleBoxFlat.new()
	bg.bg_color = Color(0.12, 0.1, 0.1)
	bg.set_corner_radius_all(3)
	var fg: StyleBoxFlat = StyleBoxFlat.new()
	fg.bg_color = fill
	fg.set_corner_radius_all(3)
	b.add_theme_stylebox_override("background", bg)
	b.add_theme_stylebox_override("fill", fg)
	var label: Label = Label.new()
	label.name = "Text"
	label.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_outline_color", Color.BLACK)
	label.add_theme_constant_override("outline_size", 4)
	b.add_child(label)
	parent.add_child(b)
	return b


func _set_bar(b: ProgressBar, value: int, max_value: int) -> void:
	b.max_value = maxf(1.0, float(max_value))
	b.value = float(value)
	(b.get_node("Text") as Label).text = "%d / %d" % [value, max_value]


func _build_player_frame() -> void:
	var p: PanelContainer = _panel(self)
	p.position = Vector2(12, 34)
	p.custom_minimum_size = Vector2(250, 0)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	p.add_child(box)
	_name_label = Label.new()
	box.add_child(_name_label)
	_hp = _bar(box, Color(0.75, 0.12, 0.1), 18)
	_end = _bar(box, Color(0.85, 0.65, 0.15), 12)
	_mana = _bar(box, Color(0.2, 0.35, 0.85), 12)
	_mana.visible = false
	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 12)
	_status.add_theme_color_override("font_color", Color(1.0, 0.7, 0.4))
	box.add_child(_status)


func _build_target_frame() -> void:
	_target_panel = _panel(self)
	_anchor(_target_panel, Vector4(0.5, 0.0, 0.5, 0.0), Vector4(-150, 34, 150, 34))
	_target_panel.grow_vertical = Control.GROW_DIRECTION_END
	_target_style = _target_panel.get_theme_stylebox("panel") as StyleBoxFlat
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	_target_panel.add_child(box)
	_target_name = Label.new()
	_target_name.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(_target_name)
	_target_hp = _bar(box, Color(0.75, 0.12, 0.1), 16)
	_target_loot = Button.new()
	_target_loot.text = "Loot (L)"
	_target_loot.pressed.connect(func() -> void: loot_requested.emit())
	box.add_child(_target_loot)
	_target_train = Button.new()
	_target_train.text = "Train (T)"
	_target_train.pressed.connect(func() -> void: trainer_requested.emit())
	box.add_child(_target_train)
	_target_panel.visible = false


func _build_hotbar() -> void:
	var holder: VBoxContainer = VBoxContainer.new()
	add_child(holder)
	_anchor(holder, Vector4(0.5, 1.0, 0.5, 1.0), Vector4(-235, -100, 385, -26))  # right of the chat window
	holder.grow_vertical = Control.GROW_DIRECTION_BEGIN
	_xp = _bar(holder, Color(0.55, 0.35, 0.85), 14)
	_xp_label = _xp.get_node("Text") as Label
	var row: HBoxContainer = HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	holder.add_child(row)
	for i: int in range(SLOT_COUNT):
		var s: Panel = HotbarSlot.new()
		s.set("index", i)
		s.set("key_text", str((i + 1) % 10))
		s.connect("activated", func(idx: int) -> void: slot_used.emit(idx))
		s.connect("changed", _on_slot_changed)
		s.connect("swapped", _on_slot_swapped)
		row.add_child(s)
		slots.append(s)
		hotbar_ids.append("")


func _window(title: String, pos: Vector2, size_px: Vector2) -> Array:
	var w: PanelContainer = _panel(self, 0.85)
	w.position = pos
	w.custom_minimum_size = size_px
	w.visible = false
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	w.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	var t: Label = Label.new()
	t.text = title
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	t.add_theme_color_override("font_color", UI.ACCENT)
	head.add_child(t)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(func() -> void: w.visible = false)
	head.add_child(close)
	return [w, box, t, close]


func _build_ability_window() -> void:
	var parts: Array = _window("Abilities (K) - drag to the hotbar", Vector2(520, 90), Vector2(380, 420))
	_abilities_win = parts[0]
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(370, 390)
	(parts[1] as VBoxContainer).add_child(scroll)
	_ability_list = VBoxContainer.new()
	_ability_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_ability_list)


func _build_inventory_window() -> void:
	var parts: Array = _window("Pack (I)", Vector2(920, 90), Vector2(260, 300))
	_inv_win = parts[0]
	_inv_coin = Label.new()
	(parts[1] as VBoxContainer).add_child(_inv_coin)
	_inv_list = VBoxContainer.new()
	(parts[1] as VBoxContainer).add_child(_inv_list)


func _build_loot_window() -> void:
	var parts: Array = _window("Loot", Vector2(900, 380), Vector2(280, 160))
	_loot_win = parts[0]
	_loot_title = parts[2]
	(parts[3] as Button).pressed.connect(func() -> void: loot_close.emit())
	_loot_list = VBoxContainer.new()
	(parts[1] as VBoxContainer).add_child(_loot_list)
	var row: HBoxContainer = HBoxContainer.new()
	(parts[1] as VBoxContainer).add_child(row)
	UI.button(row, "Take all", func() -> void: loot_take.emit(LOOT_ALL))
	UI.button(row, "Close", func() -> void: loot_close.emit())


func _build_death_dialog() -> void:
	_death = _panel(self, 0.9)
	_anchor(_death, Vector4(0.5, 0.5, 0.5, 0.5), Vector4(-210, -100, 210, 100))
	_death.grow_horizontal = Control.GROW_DIRECTION_BOTH
	_death.grow_vertical = Control.GROW_DIRECTION_BOTH
	_death.visible = false
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	_death.add_child(box)
	UI.title(box, "You have died", 24)
	_death_text = UI.label(box, "")
	UI.button(box, "Respawn at bind point", func() -> void: respawn_requested.emit())


# ------------------------------------------------------------------------------ updates

func set_stats(s: Dictionary) -> void:
	var old_level: int = level
	level = int(s["level"])
	if level != old_level and _abilities_win.visible:
		_rebuild_ability_window()  # newly learned abilities light up
	_name_label.text = "%s   level %d   AC %d" % [my_name, level, int(s["ac"])]
	_set_bar(_hp, int(s["hp"]), int(s["max_hp"]))
	_set_bar(_end, int(s["endurance"]), int(s["max_endurance"]))
	_mana.visible = int(s["max_mana"]) > 0
	if _mana.visible:
		_set_bar(_mana, int(s["mana"]), int(s["max_mana"]))
	var need: int = int(s["xp_next"])
	if need > 0:
		_xp.max_value = float(need)
		_xp.value = float(s["xp"])
		_xp_label.text = "Level %d   %d / %d XP (%d%%)" % [level, int(s["xp"]), need, int(100.0 * float(s["xp"]) / float(need))]
	else:
		_xp.max_value = 1.0
		_xp.value = 1.0
		_xp_label.text = "Level %d (maximum)" % level
	var f: int = int(s["flags"])
	var bits: Array[String] = []
	if f & 1:
		bits.append("DEAD")
	if f & 8:
		bits.append("STUNNED")
	if f & 64:
		bits.append("MESMERIZED")
	if f & 128:
		bits.append("ROOTED")
	if f & 16:
		bits.append("AFRAID")
	if f & 4:
		bits.append("sitting")
	if f & 2:
		bits.append("in combat")
	_status.text = "  ".join(bits)
	auto_attack = bool(s["auto_attack"])
	dead = (f & 1) != 0
	if not dead and _death.visible:
		_death.visible = false
	_refresh_hotbar_names()


func set_target(name_text: String, target_level: int, con: int, hp: int, kind: int) -> void:
	target_kind = kind
	_target_panel.visible = true
	var c: Color = CON_COLORS[clampi(con, 0, CON_COLORS.size() - 1)]
	_target_name.text = "%s   (level %d)" % [name_text, target_level]
	_target_name.add_theme_color_override("font_color", c)
	_target_style.border_color = c
	_target_style.set_border_width_all(2)
	_target_hp.visible = kind != 2
	_target_loot.visible = kind == 2
	_target_train.visible = kind == KIND_NPC
	set_target_hp(hp)


func set_target_hp(hp: int) -> void:
	_target_hp.max_value = 100.0
	_target_hp.value = float(hp)
	(_target_hp.get_node("Text") as Label).text = "%d%%" % hp


func clear_target() -> void:
	target_kind = -1
	_target_panel.visible = false


static func con_name(con: int) -> String:
	return CON_NAMES[clampi(con, 0, CON_NAMES.size() - 1)]


func set_abilities(list: Array, char_name: String) -> void:
	abilities.clear()
	ability_order.clear()
	for a: Dictionary in list:
		abilities[str(a["id"])] = a
		ability_order.append(str(a["id"]))
	_save_path = "user://hotbar_%s.cfg" % char_name.to_lower()
	_load_hotbar()
	_rebuild_ability_window()
	_refresh_hotbar_names()


func _rebuild_ability_window() -> void:
	for c: Node in _ability_list.get_children():
		c.queue_free()
	for id: String in ability_order:
		var a: Dictionary = abilities[id]
		var row: PanelContainer = AbilityRow.new()
		row.call("setup", a, int(a["level"]) <= level)
		row.connect("use_requested", func(aid: String) -> void: ability_used.emit(aid))
		_ability_list.add_child(row)


func ability_name(id: String) -> String:
	if id == ATTACK_ID:
		return "Attack"
	if id.begins_with(GEM_PREFIX):
		var n: int = id.substr(GEM_PREFIX.length()).to_int()
		var spell: String = str(spells.call("gem_spell_name", n))
		return spell if spell != "" else "Gem %d" % (n + 1)
	if abilities.has(id):
		return str((abilities[id] as Dictionary)["name"])
	return id


func _refresh_hotbar_names() -> void:
	for i: int in range(SLOT_COUNT):
		var id: String = hotbar_ids[i]
		var usable: bool = id == ATTACK_ID or (abilities.has(id) and int((abilities[id] as Dictionary)["level"]) <= level) \
			or (id.begins_with(GEM_PREFIX) and str(spells.call("gem_spell", id.substr(GEM_PREFIX.length()).to_int())) != "")
		slots[i].call("show_ability", id, ability_name(id) if id != "" else "", usable)


func refresh_hotbar() -> void:
	_refresh_hotbar_names()


func _on_slot_changed(index: int, ability_id: String) -> void:
	hotbar_ids[index] = ability_id
	_refresh_hotbar_names()
	_save_hotbar()


func _on_slot_swapped(from_index: int, to_index: int) -> void:
	var moved: String = hotbar_ids[from_index]
	hotbar_ids[from_index] = hotbar_ids[to_index]
	hotbar_ids[to_index] = moved
	_refresh_hotbar_names()
	_save_hotbar()


func _load_hotbar() -> void:
	var cfg: ConfigFile = ConfigFile.new()
	var ids: Array = []
	if cfg.load(_save_path) == OK:
		var saved: Variant = cfg.get_value("hotbar", "slots", [])
		if saved is Array:  # ignore a hand-edited or corrupt value
			ids = saved
	if ids.size() != SLOT_COUNT:
		ids = []
		# A class with no abilities of its own is a caster: its bar casts gems.
		for id: String in (DEFAULT_BAR if not abilities.is_empty() else CASTER_BAR):
			ids.append(id if _bindable(id) else "")
	for i: int in range(SLOT_COUNT):
		var id: String = str(ids[i])
		hotbar_ids[i] = id if id == "" or _bindable(id) else ""


func _bindable(id: String) -> bool:
	if id == ATTACK_ID or abilities.has(id):
		return true
	if id.begins_with(GEM_PREFIX):
		var n: int = id.substr(GEM_PREFIX.length()).to_int()
		return n >= 0 and n < 8
	return false


func _save_hotbar() -> void:
	if _save_path == "":
		return
	var cfg: ConfigFile = ConfigFile.new()
	cfg.set_value("hotbar", "slots", hotbar_ids)
	var err: Error = cfg.save(_save_path)
	if err != OK:
		notice.emit("Could not save your hotbar layout (%s); it will reset next time." % error_string(err))


## Records cooldowns from an AbilityResult (success or a refusal that reports time left).
func note_ability_result(r: Dictionary) -> void:
	var now: int = Time.get_ticks_msec()
	var id: String = str(r["ability_id"])
	var cd: int = int(r["cooldown_ms"])
	var gcd: int = int(r["gcd_ms"])
	if cd > 0:
		cooldown_end[id] = now + cd
		if bool(r["ok"]) or not cooldown_total.has(id):
			cooldown_total[id] = float(cd) / 1000.0
	if gcd > 0:
		gcd_end = now + gcd
		if bool(r["ok"]):
			gcd_total = float(gcd) / 1000.0


func _process(_delta: float) -> void:
	var now: int = Time.get_ticks_msec()
	for i: int in range(SLOT_COUNT):
		var id: String = hotbar_ids[i]
		var remaining: float = 0.0
		var total: float = 0.0
		if id.begins_with(GEM_PREFIX):
			var lock: Array = spells.call("gem_lock", id.substr(GEM_PREFIX.length()).to_int())
			remaining = float(lock[0])
			total = float(lock[1])
		elif id != "" and id != ATTACK_ID:
			var cd_left: float = float(int(cooldown_end.get(id, 0)) - now) / 1000.0
			var uses_gcd: bool = abilities.has(id) and bool((abilities[id] as Dictionary)["gcd"])
			var gcd_left: float = float(gcd_end - now) / 1000.0 if uses_gcd else 0.0
			if cd_left > gcd_left and cd_left > 0.0:
				remaining = cd_left
				total = float(cooldown_total.get(id, cd_left))
			elif gcd_left > 0.0:
				remaining = gcd_left
				total = gcd_total
		slots[i].call("show_state", remaining, total, id == ATTACK_ID and auto_attack)


func toggle_abilities() -> void:
	_abilities_win.visible = not _abilities_win.visible
	if _abilities_win.visible:
		_rebuild_ability_window()


func toggle_inventory() -> void:
	_inv_win.visible = not _inv_win.visible


func set_inventory(inv: Dictionary) -> void:
	inventory_coin = int(inv["coin"])
	inventory_items = inv["items"]
	_inv_coin.text = "Coin: %s" % format_coin(inventory_coin)
	spells.call("set_coin", inventory_coin)
	for c: Node in _inv_list.get_children():
		c.queue_free()
	if inventory_items.is_empty():
		UI.label(_inv_list, "(empty)")
	for it: Dictionary in inventory_items:
		var row: HBoxContainer = HBoxContainer.new()
		_inv_list.add_child(row)
		var l: Label = Label.new()
		l.text = "%s%s" % [str(it["name"]), (" x%d" % int(it["count"])) if int(it["count"]) > 1 else ""]
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(l)
		if str(it.get("kind", "")) == "scroll":
			var b: Button = Button.new()
			b.text = "Scribe"
			b.tooltip_text = "Write this spell into your spellbook (uses up the scroll)"
			var slot: int = int(it["slot"])
			b.pressed.connect(func() -> void: scribe_requested.emit(slot))
			row.add_child(b)


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


func show_loot(w: Dictionary) -> void:
	loot_corpse = int(w["corpse_id"])
	loot_items = w["items"]
	_loot_title.text = str(w["name"])
	for c: Node in _loot_list.get_children():
		c.queue_free()
	if loot_items.is_empty():
		UI.label(_loot_list, "Nothing left on this corpse.")
	for it: Dictionary in loot_items:
		var row: HBoxContainer = HBoxContainer.new()
		_loot_list.add_child(row)
		var l: Label = Label.new()
		l.text = "%s%s" % [str(it["name"]), (" x%d" % int(it["count"])) if int(it["count"]) > 1 else ""]
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(l)
		var slot: int = int(it["slot"])
		var b: Button = Button.new()
		b.text = "Take"
		b.pressed.connect(func() -> void: loot_take.emit(slot))
		row.add_child(b)
	_loot_win.visible = true


func hide_loot() -> void:
	loot_corpse = 0
	loot_items = []
	_loot_win.visible = false


func is_loot_open() -> bool:
	return _loot_win.visible


func show_death(killer: String, xp_lost: int) -> void:
	dead = true
	var lost: String = "You lost %d experience." % xp_lost if xp_lost > 0 else "You lost no experience."
	_death_text.text = "You were slain by %s.\n%s\nYour corpse, with your coin and pack, lies where you fell until it decays. Return to it soon and loot it to recover your belongings." % [killer, lost]
	_death.visible = true


func is_death_shown() -> bool:
	return _death.visible


func any_window_open() -> bool:
	return _abilities_win.visible or _inv_win.visible or _loot_win.visible or bool(spells.call("any_window_open"))


func close_windows() -> void:
	_abilities_win.visible = false
	_inv_win.visible = false
	spells.call("close_windows")
	if _loot_win.visible:
		loot_close.emit()
