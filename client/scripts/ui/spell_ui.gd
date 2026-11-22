## The spell half of the HUD: the gem bar (8 gems above the hotbar), the cast bar, your buff tray
## (top right), the target's effects (under the target frame), the spellbook window (B), a
## trainer's window and the resurrection prompt. (The group window lives in social_ui.gd.)
## Like the rest of the HUD it only presents state and emits intents.
extends Control

signal cast_gem(gem: int)
signal memorize(gem: int, spell_id: String)
signal forget(gem: int)
signal trainer_buy(npc_id: int, item_id: String)
signal rez_answer(offer_id: int, accept: bool)
signal notice(text: String)

const UI := preload("res://scripts/ui/ui.gd")
const GemSlot := preload("res://scripts/ui/gem_slot.gd")
const SpellRow := preload("res://scripts/ui/spell_row.gd")
const BuffIcon := preload("res://scripts/ui/buff_icon.gd")

const GEM_COUNT: int = 8
const RESULT_DONE: int = 0
const RESULT_INTERRUPTED: int = 1
const RESULT_CANCELLED: int = 2

var spells: Dictionary = {}  # id -> SpellInfo dictionary (our spellbook)
var spell_order: Array[String] = []
var book_capacity: int = 0
var unlocked: int = 0
var gem_ids: Array[String] = []
var casting: bool = false
var cast_spell_id: String = ""
var self_buffs: Array = []    # BuffInfo dictionaries
var target_buffs: Array = []
var target_buff_entity: int = 0
var trainer_npc: int = 0
var trainer_entries: Array = []
var rez_pending: bool = false
var rez_offer_id: int = 0
var coin: int = 0
# Counters the autotest reports.
var casts_completed: int = 0
var casts_interrupted: int = 0

var _gems: Array = []  # GemSlot nodes
var _gem_ready_end: Array[int] = []
var _gem_ready_total: Array[float] = []
var _cooldown_end: Dictionary = {}
var _cooldown_total: Dictionary = {}
var _gcd_end: int = 0
var _gcd_total: float = 1.5
var _cast_bar: ProgressBar
var _cast_label: Label
var _cast_start: int = 0
var _cast_ms: int = 0
var _cast_hide_at: int = 0
var _tray: HFlowContainer
var _target_tray: HFlowContainer
var _book_win: PanelContainer
var _book_title: Label
var _book_list: VBoxContainer
var _trainer_win: PanelContainer
var _trainer_title: Label
var _trainer_greeting: Label
var _trainer_coin: Label
var _trainer_list: VBoxContainer
var _rez: PanelContainer
var _rez_text: Label
var _rez_ends: int = 0


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	for i: int in range(GEM_COUNT):
		gem_ids.append("")
		_gem_ready_end.append(0)
		_gem_ready_total.append(0.0)
	_build_gems()
	_build_cast_bar()
	_build_trays()
	_build_spellbook()
	_build_trainer()
	_build_rez()


# ------------------------------------------------------------------------------ construction

static func _anchor(c: Control, anchors: Vector4, offsets: Vector4) -> void:
	c.anchor_left = anchors.x
	c.anchor_top = anchors.y
	c.anchor_right = anchors.z
	c.anchor_bottom = anchors.w
	c.offset_left = offsets.x
	c.offset_top = offsets.y
	c.offset_right = offsets.z
	c.offset_bottom = offsets.w


func _panel(alpha: float) -> PanelContainer:
	var p: PanelContainer = PanelContainer.new()
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = Color(0.05, 0.04, 0.06, alpha)
	s.border_color = Color(0.35, 0.3, 0.45)
	s.set_border_width_all(1)
	s.set_corner_radius_all(4)
	s.set_content_margin_all(8)
	p.add_theme_stylebox_override("panel", s)
	add_child(p)
	return p


func _window(title: String, pos: Vector2, size_px: Vector2) -> Array:
	var w: PanelContainer = _panel(0.88)
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
	t.add_theme_color_override("font_color", Color(0.75, 0.65, 1.0))
	head.add_child(t)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(func() -> void: w.visible = false)
	head.add_child(close)
	return [w, box, t]


func _build_gems() -> void:
	var row: HBoxContainer = HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	add_child(row)
	_anchor(row, Vector4(0.5, 1.0, 0.5, 1.0), Vector4(-140, -154, 290, -106))  # above the hotbar
	for i: int in range(GEM_COUNT):
		var g: Panel = GemSlot.new()
		g.set("gem", i)
		g.connect("cast_requested", func(n: int) -> void: cast_gem.emit(n))
		g.connect("memorize_requested", func(n: int, id: String) -> void: memorize.emit(n, id))
		g.connect("forget_requested", func(n: int) -> void: forget.emit(n))
		row.add_child(g)
		_gems.append(g)
	row.visible = false  # shown once we know a spellbook exists
	row.name = "GemRow"


func _gem_row() -> Control:
	return get_node("GemRow") as Control


func _build_cast_bar() -> void:
	var holder: VBoxContainer = VBoxContainer.new()
	add_child(holder)
	_anchor(holder, Vector4(0.5, 1.0, 0.5, 1.0), Vector4(-90, -196, 240, -160))
	holder.name = "CastHolder"
	_cast_bar = ProgressBar.new()
	_cast_bar.custom_minimum_size = Vector2(0, 18)
	_cast_bar.show_percentage = false
	var bg: StyleBoxFlat = StyleBoxFlat.new()
	bg.bg_color = Color(0.1, 0.08, 0.14, 0.9)
	bg.set_corner_radius_all(3)
	var fg: StyleBoxFlat = StyleBoxFlat.new()
	fg.bg_color = Color(0.55, 0.45, 1.0)
	fg.set_corner_radius_all(3)
	_cast_bar.add_theme_stylebox_override("background", bg)
	_cast_bar.add_theme_stylebox_override("fill", fg)
	holder.add_child(_cast_bar)
	_cast_label = Label.new()
	_cast_label.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_cast_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_cast_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_cast_label.add_theme_font_size_override("font_size", 12)
	_cast_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_cast_label.add_theme_constant_override("outline_size", 4)
	_cast_bar.add_child(_cast_label)
	holder.visible = false


func _cast_holder() -> Control:
	return get_node("CastHolder") as Control


func _build_trays() -> void:
	_tray = HFlowContainer.new()
	_tray.add_theme_constant_override("h_separation", 4)
	_tray.add_theme_constant_override("v_separation", 2)
	_tray.alignment = FlowContainer.ALIGNMENT_END
	add_child(_tray)
	_anchor(_tray, Vector4(1.0, 0.0, 1.0, 0.0), Vector4(-336, 34, -12, 140))
	_target_tray = HFlowContainer.new()
	_target_tray.add_theme_constant_override("h_separation", 4)
	_target_tray.alignment = FlowContainer.ALIGNMENT_CENTER
	add_child(_target_tray)
	_anchor(_target_tray, Vector4(0.5, 0.0, 0.5, 0.0), Vector4(-150, 128, 150, 180))  # under the target frame


func _build_spellbook() -> void:
	var parts: Array = _window("Spellbook (B) - drag a spell onto a gem", Vector2(470, 70), Vector2(440, 470))
	_book_win = parts[0]
	_book_title = Label.new()
	_book_title.add_theme_font_size_override("font_size", 12)
	(parts[1] as VBoxContainer).add_child(_book_title)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(430, 410)
	(parts[1] as VBoxContainer).add_child(scroll)
	_book_list = VBoxContainer.new()
	_book_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_book_list)


func _build_trainer() -> void:
	var parts: Array = _window("Trainer", Vector2(930, 70), Vector2(330, 460))
	_trainer_win = parts[0]
	_trainer_title = parts[2]
	_trainer_greeting = UI.label(parts[1], "")
	_trainer_greeting.add_theme_font_size_override("font_size", 12)
	_trainer_greeting.custom_minimum_size = Vector2(310, 0)
	_trainer_coin = Label.new()
	_trainer_coin.add_theme_color_override("font_color", Color(1.0, 0.85, 0.4))
	(parts[1] as VBoxContainer).add_child(_trainer_coin)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(320, 340)
	(parts[1] as VBoxContainer).add_child(scroll)
	_trainer_list = VBoxContainer.new()
	_trainer_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_trainer_list)


func _build_rez() -> void:
	_rez = _panel(0.92)
	_anchor(_rez, Vector4(0.5, 0.5, 0.5, 0.5), Vector4(-200, -180, 200, -60))
	_rez.grow_horizontal = Control.GROW_DIRECTION_BOTH
	_rez.visible = false
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	_rez.add_child(box)
	UI.title(box, "Resurrection", 20)
	_rez_text = UI.label(box, "")
	var row: HBoxContainer = UI.row(box)
	UI.button(row, "Accept", func() -> void: _answer_rez(true))
	UI.button(row, "Decline", func() -> void: _answer_rez(false))


# ------------------------------------------------------------------------------ spellbook and gems

func set_spellbook(msg: Dictionary) -> void:
	spells.clear()
	spell_order.clear()
	book_capacity = int(msg["capacity"])
	for s: Dictionary in msg["spells"]:
		spells[str(s["id"])] = s
		spell_order.append(str(s["id"]))
	_gem_row().visible = not spells.is_empty()
	_refresh_gems()
	if _book_win.visible:
		_rebuild_book()


func has_spells() -> bool:
	return not spells.is_empty()


func set_gems(msg: Dictionary) -> void:
	unlocked = int(msg["unlocked"])
	var now: int = Time.get_ticks_msec()
	var list: Array = msg["gems"]
	for i: int in range(GEM_COUNT):
		var g: Dictionary = list[i] if i < list.size() else {"spell_id": "", "ready_ms": 0}
		gem_ids[i] = str(g["spell_id"])
		var ready: int = int(g["ready_ms"])
		_gem_ready_end[i] = now + ready
		_gem_ready_total[i] = maxf(0.001, float(ready) / 1000.0)
	_refresh_gems()
	if _book_win.visible:
		_rebuild_book()


func _refresh_gems() -> void:
	for i: int in range(GEM_COUNT):
		var id: String = gem_ids[i]
		var s: Dictionary = spells.get(id, {})
		_gems[i].call("show_spell", id, str(s.get("name", id)), int(s.get("resist", 0)), i < unlocked, str(s.get("description", "")))


func gem_spell(gem: int) -> String:
	return gem_ids[gem] if gem >= 0 and gem < GEM_COUNT else ""


func gem_spell_name(gem: int) -> String:
	var id: String = gem_spell(gem)
	return str((spells.get(id, {}) as Dictionary).get("name", "")) if id != "" else ""


func spell_name(id: String) -> String:
	return str((spells.get(id, {}) as Dictionary).get("name", id))


func spell_description(id: String) -> String:
	return str((spells.get(id, {}) as Dictionary).get("description", ""))


## The first empty unlocked gem, or -1.
func free_gem() -> int:
	for i: int in range(mini(unlocked, GEM_COUNT)):
		if gem_ids[i] == "":
			return i
	return -1


func toggle_spellbook() -> void:
	_book_win.visible = not _book_win.visible
	if _book_win.visible:
		_rebuild_book()


func _rebuild_book() -> void:
	for c: Node in _book_list.get_children():
		c.queue_free()
	_book_title.text = "%d / %d spells known   ·   %d of %d gems open" % [spells.size(), book_capacity, unlocked, GEM_COUNT]
	if spells.is_empty():
		UI.label(_book_list, "Your spellbook is empty. Spell scrolls come from trainers and from loot.")
	for id: String in spell_order:
		var s: Dictionary = spells[id]
		var gem: int = gem_ids.find(id)
		var row: PanelContainer = SpellRow.new()
		row.call("setup", s, true, str(gem + 1) if gem >= 0 else "")
		row.connect("memorize_requested", _memorize_first_free)
		_book_list.add_child(row)


func _memorize_first_free(spell_id: String) -> void:
	if gem_ids.has(spell_id):
		notice.emit("%s is already memorized." % spell_name(spell_id))
		return
	var g: int = free_gem()
	if g < 0:
		notice.emit("All your open gems hold spells: drag %s onto one to replace it." % spell_name(spell_id))
		return
	memorize.emit(g, spell_id)


## Cooldowns from an AbilityResult for one of our spells (recast and the global cooldown).
func note_ability_result(r: Dictionary) -> void:
	var id: String = str(r["ability_id"])
	if not spells.has(id):
		return
	var now: int = Time.get_ticks_msec()
	var cd: int = int(r["cooldown_ms"])
	var gcd: int = int(r["gcd_ms"])
	if cd > 0:
		_cooldown_end[id] = now + cd
		if bool(r["ok"]) or not _cooldown_total.has(id):
			_cooldown_total[id] = float(cd) / 1000.0
	if gcd > 0:
		_gcd_end = now + gcd
		if bool(r["ok"]):
			_gcd_total = float(gcd) / 1000.0


## [remaining, total] seconds before a gem can cast (memorize, recast, or global cooldown).
func gem_lock(gem: int) -> Array:
	var now: int = Time.get_ticks_msec()
	var id: String = gem_spell(gem)
	if id == "":
		return [0.0, 0.0]
	var best: Array = [0.0, 0.0]
	var mem_left: float = float(_gem_ready_end[gem] - now) / 1000.0
	if mem_left > 0.0:
		best = [mem_left, _gem_ready_total[gem]]
	var cd_left: float = float(int(_cooldown_end.get(id, 0)) - now) / 1000.0
	if cd_left > float(best[0]):
		best = [cd_left, float(_cooldown_total.get(id, cd_left))]
	var gcd_left: float = float(_gcd_end - now) / 1000.0
	if gcd_left > float(best[0]):
		best = [gcd_left, _gcd_total]
	return best


# ------------------------------------------------------------------------------ casting

func cast_started(msg: Dictionary) -> void:
	casting = true
	cast_spell_id = str(msg["spell_id"])
	_cast_start = Time.get_ticks_msec()
	_cast_ms = maxi(1, int(msg["cast_ms"]))
	_cast_hide_at = 0
	_cast_bar.max_value = 1.0
	_cast_bar.value = 0.0
	(_cast_bar.get_theme_stylebox("fill") as StyleBoxFlat).bg_color = Color(0.55, 0.45, 1.0)
	_cast_label.text = str(msg["spell_name"])
	_cast_holder().visible = true


func cast_ended(msg: Dictionary) -> void:
	casting = false
	var result: int = int(msg["result"])
	var fill: StyleBoxFlat = _cast_bar.get_theme_stylebox("fill") as StyleBoxFlat
	if result == RESULT_DONE:
		casts_completed += 1
		_cast_bar.value = 1.0
		_cast_hide_at = Time.get_ticks_msec() + 150
		return
	if result == RESULT_INTERRUPTED:
		casts_interrupted += 1
	fill.bg_color = Color(0.85, 0.2, 0.2) if result == RESULT_INTERRUPTED else Color(0.45, 0.45, 0.45)
	_cast_bar.value = 1.0
	var why: String = str(msg["message"])
	_cast_label.text = why if why != "" else ("Interrupted" if result == RESULT_INTERRUPTED else "Stopped")
	_cast_hide_at = Time.get_ticks_msec() + 1200


# ------------------------------------------------------------------------------ buffs

func set_buffs(entity_id: int, list: Array, mine: bool) -> void:
	if mine:
		self_buffs = list
		_fill_tray(_tray, list)
	else:
		target_buffs = list
		target_buff_entity = entity_id
		_fill_tray(_target_tray, list)


func clear_target_buffs() -> void:
	target_buffs = []
	target_buff_entity = 0
	_fill_tray(_target_tray, [])


func _fill_tray(tray: HFlowContainer, list: Array) -> void:
	for c: Node in tray.get_children():
		c.queue_free()
	var now: int = Time.get_ticks_msec()
	for b: Dictionary in list:
		var icon: VBoxContainer = BuffIcon.new()
		icon.call("setup", b, spell_description(str(b["spell_id"])), now)
		tray.add_child(icon)


func has_self_buff(spell_id: String) -> bool:
	for b: Dictionary in self_buffs:
		if str(b["spell_id"]) == spell_id:
			return true
	return false


# ------------------------------------------------------------------------------ trainer

func show_trainer(msg: Dictionary) -> void:
	trainer_npc = int(msg["npc_id"])
	trainer_entries = msg["entries"]
	_trainer_title.text = "%s - trainer" % str(msg["name"])
	_trainer_greeting.text = "\"%s\"" % str(msg["greeting"]) if str(msg["greeting"]) != "" else ""
	_rebuild_trainer()
	_trainer_win.visible = true


func _rebuild_trainer() -> void:
	for c: Node in _trainer_list.get_children():
		c.queue_free()
	_trainer_coin.text = "Your coin: %s" % _coin_text(coin)
	for e: Dictionary in trainer_entries:
		var row: HBoxContainer = HBoxContainer.new()
		_trainer_list.add_child(row)
		var l: Label = Label.new()
		var known: bool = bool(e["known"]) or spells.has(str(e["spell_id"]))
		l.text = "L%d  %s" % [int(e["level"]), str(e["name"])]
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		l.modulate = Color(0.55, 0.55, 0.55) if known else Color.WHITE
		row.add_child(l)
		var price: Label = Label.new()
		price.text = "known" if known else _coin_text(int(e["price"]))
		price.add_theme_color_override("font_color", Color(1.0, 0.85, 0.4))
		row.add_child(price)
		var b: Button = Button.new()
		b.text = "Buy"
		b.disabled = known or int(e["price"]) > coin
		var item_id: String = str(e["item_id"])
		b.pressed.connect(func() -> void: trainer_buy.emit(trainer_npc, item_id))
		row.add_child(b)


func set_coin(c: int) -> void:
	coin = c
	if _trainer_win.visible:
		_rebuild_trainer()


func is_trainer_open() -> bool:
	return _trainer_win.visible


func close_trainer() -> void:
	_trainer_win.visible = false


static func _coin_text(copper: int) -> String:
	var parts: Array[String] = []
	if copper >= 1000:
		parts.append("%dp" % (copper / 1000))
	if (copper / 100) % 10 > 0:
		parts.append("%dg" % ((copper / 100) % 10))
	if (copper / 10) % 10 > 0:
		parts.append("%ds" % ((copper / 10) % 10))
	if copper % 10 > 0 or parts.is_empty():
		parts.append("%dc" % (copper % 10))
	return " ".join(parts)


# ------------------------------------------------------------------------------ resurrection

func show_rez(msg: Dictionary) -> void:
	rez_pending = true
	rez_offer_id = int(msg["offer_id"])
	_rez_ends = Time.get_ticks_msec() + int(msg["expires_ms"])
	_rez_text.text = "%s wants to resurrect you, restoring %d%% of the experience your death cost.\nYou will return to your corpse." % [
		str(msg["caster"]), int(msg["pct"])]
	_rez.visible = true


func is_rez_shown() -> bool:
	return _rez.visible


func _answer_rez(accept: bool) -> void:
	_rez.visible = false
	rez_pending = false
	rez_answer.emit(rez_offer_id, accept)


func any_window_open() -> bool:
	return _book_win.visible or _trainer_win.visible


func close_windows() -> void:
	_book_win.visible = false
	_trainer_win.visible = false


# ------------------------------------------------------------------------------ per frame

func _process(_delta: float) -> void:
	var now: int = Time.get_ticks_msec()
	for i: int in range(GEM_COUNT):
		var lock: Array = gem_lock(i)
		_gems[i].call("show_state", float(lock[0]), float(lock[1]), casting)
	if casting:
		_cast_bar.value = clampf(float(now - _cast_start) / float(_cast_ms), 0.0, 1.0)
	elif _cast_hide_at != 0 and now >= _cast_hide_at:
		_cast_hide_at = 0
		_cast_holder().visible = false
	for icon: Node in _tray.get_children():
		icon.call("refresh")
	for icon: Node in _target_tray.get_children():
		icon.call("refresh")
	if _rez.visible and now >= _rez_ends:
		_rez.visible = false
		rez_pending = false
