## The crafting station window (Phase 13). It shows the station's tradeskill and your skill in it, the
## tradeskill's recipes (searchable, with what you can make now and trivial recipes filtered on request,
## coloured by how much each still teaches), and the chosen recipe's ingredients and tools against what
## your pack holds. Combine sends the intent, once or a few times in a row; the server decides everything.
## Recipes come from client/data/crafts.json (synced from content/crafts.json).
extends Control

signal combine(npc_id: int, recipe_id: String, request: int)

const UI := preload("res://scripts/ui/ui.gd")
const ItemDb := preload("res://scripts/ui/item_db.gd")
const DATA_PATH: String = "res://data/crafts.json"
## The most rows the list draws at once (smithing alone has hundreds): search to narrow it.
const MAX_ROWS: int = 150

## How much a recipe still teaches, by its trivial against your skill.
const TRIVIAL: int = 0  # grey: nothing left to learn
const EASY: int = 1     # green
const FAIR: int = 2     # yellow
const HARD: int = 3     # red: likely to fail
const DIFFICULTY_COLORS: Array[Color] = [Color(0.6, 0.6, 0.6), Color(0.45, 0.9, 0.45), Color(1.0, 0.85, 0.35), Color(1.0, 0.45, 0.4)]

static var _data: Dictionary = {}

var npc_id: int = 0
var tradeskill: String = ""
var station_name: String = ""
## Tradeskill id -> {"id", "name", "value", "cap"} (CraftSkills).
var skills: Dictionary = {}
## Item id -> how many the pack holds (the eight pack slots and the bags in them, never the bank or worn gear).
var pack: Dictionary = {}
var selected: String = ""
var search_text: String = ""
var only_makeable: bool = false
var hide_trivial: bool = false
## Combines still to send after the one in flight ("Combine" with a count above one).
var repeat_left: int = 0
var _waiting: bool = false
## The number of the combine in flight (echoed by its CraftResult): a late result of an older one changes nothing.
var _pending_request: int = 0
var _next_request: int = 0
var _sent_at_ms: int = 0
var _next_at_ms: int = 0
var _win: PanelContainer
var _title: Label
var _skill_label: Label
var _search: LineEdit
var _makeable_box: CheckBox
var _trivial_box: CheckBox
var _list: VBoxContainer
var _detail: Label
var _count: SpinBox
var _combine_btn: Button
var _status: Label


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build()


# ------------------------------------------------------------------------------ data

static func data() -> Dictionary:
	if _data.is_empty():
		var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(DATA_PATH))
		if parsed is Dictionary:
			_data = parsed
	return _data


static func tradeskill_def(id: String) -> Dictionary:
	for t: Variant in data().get("tradeskills", []):
		if str((t as Dictionary)["id"]) == id:
			return t
	return {}


static func recipe(id: String) -> Dictionary:
	for r: Variant in data().get("recipes", []):
		if str((r as Dictionary)["id"]) == id:
			return r
	return {}


## A tradeskill's recipes, easiest first.
static func recipes_for(ts: String) -> Array:
	var out: Array = []
	for r: Variant in data().get("recipes", []):
		if str((r as Dictionary)["tradeskill"]) == ts:
			out.append(r)
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return int(a["trivial"]) < int(b["trivial"]) or (int(a["trivial"]) == int(b["trivial"]) and str(a["id"]) < str(b["id"])))
	return out


static func difficulty(skill: int, trivial: int) -> int:
	if trivial <= skill:
		return TRIVIAL
	if trivial - skill <= 20:
		return EASY
	if trivial - skill <= 50:
		return FAIR
	return HARD


## The server's combine chance (game/crafting.cpp), for display.
static func success_chance(skill: int, trivial: int) -> float:
	var r: Dictionary = data().get("rules", {})
	var hi: float = float(r.get("success_max", 0.95))
	var lo: float = float(r.get("success_min", 0.05))
	return clampf(hi - float(maxi(0, trivial - skill)) * float(r.get("success_per_point", 0.012)), lo, hi)


## Pack counts from an Inventory message's items (slot codes as the server's game/items.hpp).
static func pack_counts(items: Array) -> Dictionary:
	var out: Dictionary = {}
	for it: Variant in items:
		var d: Dictionary = it as Dictionary
		var cont: int = int(d["slot"]) >> 8
		if cont == ItemDb.CONT_GENERAL or (cont >= ItemDb.CONT_GENERAL_BAG and cont < ItemDb.CONT_GENERAL_BAG + ItemDb.GENERAL_SLOTS):
			var id: String = str(d["item_id"])
			out[id] = int(out.get(id, 0)) + int(d["count"])
	return out


func skill_in(ts: String) -> int:
	return int((skills.get(ts, {}) as Dictionary).get("value", 0))


func have(item_id: String) -> int:
	return int(pack.get(item_id, 0))


## Everything the recipe needs is in the pack.
func can_make(r: Dictionary) -> bool:
	for i: Variant in r.get("ingredients", []):
		if have(str((i as Dictionary)["item"])) < int((i as Dictionary)["count"]):
			return false
	for t: Variant in r.get("tools", []):
		if have(str(t)) < 1:
			return false
	return true


## The recipes the list shows, after the search and the two filters.
func visible_recipes() -> Array:
	var out: Array = []
	var skill: int = skill_in(tradeskill)
	var needle: String = search_text.strip_edges().to_lower()
	for r: Variant in recipes_for(tradeskill):
		var rd: Dictionary = r as Dictionary
		if hide_trivial and int(rd["trivial"]) <= skill:
			continue
		if only_makeable and not can_make(rd):
			continue
		if needle != "" and not ItemDb.name_of(str((rd["result"] as Dictionary)["item"])).to_lower().contains(needle):
			continue
		out.append(rd)
	return out


## What the detail pane says about a recipe.
func describe(id: String) -> String:
	var r: Dictionary = recipe(id)
	if r.is_empty():
		return "Choose a recipe."
	var res: Dictionary = r["result"]
	var skill: int = skill_in(str(r["tradeskill"]))
	var trivial: int = int(r["trivial"])
	var lines: PackedStringArray = []
	var made: String = ItemDb.name_of(str(res["item"]))
	lines.append(("%d x %s" % [int(res["count"]), made]) if int(res["count"]) > 1 else made)
	if trivial <= skill:
		lines.append("Trivial at %d: you will learn nothing more from it (%d%% success)." % [trivial, roundi(success_chance(skill, trivial) * 100.0)])
	else:
		lines.append("Trivial at %d, your skill %d: about %d%% success." % [trivial, skill, roundi(success_chance(skill, trivial) * 100.0)])
	lines.append("Ingredients:")
	for i: Variant in r.get("ingredients", []):
		var iid: String = str((i as Dictionary)["item"])
		var need: int = int((i as Dictionary)["count"])
		lines.append("  %d x %s (you have %d)%s" % [need, ItemDb.name_of(iid), have(iid), "" if have(iid) >= need else "  - not enough"])
	var tools: Array = r.get("tools", [])
	if not tools.is_empty():
		lines.append("Tools (kept):")
		for t: Variant in tools:
			lines.append("  %s%s" % [ItemDb.name_of(str(t)), "" if have(str(t)) > 0 else "  - not in your pack"])
	return "\n".join(lines)


# ------------------------------------------------------------------------------ server state

## CraftWindow: open at a station, or close (npc_id 0).
func show_station(msg: Dictionary) -> void:
	if int(msg["npc_id"]) == 0:
		close_windows()
		return
	var changed: bool = int(msg["npc_id"]) != npc_id
	npc_id = int(msg["npc_id"])
	tradeskill = str(msg["tradeskill"])
	station_name = str(msg["name"])
	if changed:
		selected = ""
		repeat_left = 0
		_waiting = false
		_status.text = ""
	_win.visible = true
	_refresh()


## CraftSkills: every tradeskill (announce = only the ones that rose).
func set_craft_skills(msg: Dictionary) -> void:
	if not bool(msg["announce"]):
		skills.clear()
	for s: Variant in msg["skills"]:
		skills[str((s as Dictionary)["id"])] = s
	if _win.visible:
		_refresh()


## Inventory: what the pack holds now.
func set_inventory(inv: Dictionary) -> void:
	pack = pack_counts(inv["items"])
	if _win.visible:
		_refresh()


## CraftResult: report it, and send the next of a run of combines once the server will take it.
func on_result(msg: Dictionary) -> void:
	var ours: bool = _waiting and int(msg.get("request", -1)) == _pending_request
	if not ours:
		return  # the answer to a combine sent before the window closed, or at another station
	_waiting = false
	var made: String = ItemDb.name_of(str((recipe(str(msg["recipe_id"])).get("result", {}) as Dictionary).get("item", "")))
	if bool(msg["success"]):
		_status.text = "Made %s." % (("%d x %s" % [int(msg["made"]), made]) if int(msg["made"]) > 1 else made)
		_status.add_theme_color_override("font_color", UI.OK_COLOR)
	else:
		_status.text = "Failed to make %s." % made
		_status.add_theme_color_override("font_color", UI.ERROR_COLOR)
	if repeat_left > 0:
		var secs: float = float((data().get("rules", {}) as Dictionary).get("combine_seconds", 1.5))
		_next_at_ms = Time.get_ticks_msec() + int(secs * 1000.0) + 100


func any_window_open() -> bool:
	return _win.visible


func close_windows() -> void:
	_win.visible = false
	npc_id = 0
	repeat_left = 0
	_waiting = false


## Combine the chosen recipe `times` times in a row (stops early when something runs out).
func start_combine(times: int = 1) -> void:
	if npc_id == 0 or selected == "" or _waiting:
		return
	repeat_left = maxi(0, times - 1)
	_send()


func _send() -> void:
	var r: Dictionary = recipe(selected)
	if r.is_empty() or not can_make(r):
		repeat_left = 0
		_status.text = "You are missing something for that."
		_status.add_theme_color_override("font_color", UI.ERROR_COLOR)
		return
	_waiting = true
	_next_request = (_next_request + 1) % 65536
	_pending_request = _next_request
	_sent_at_ms = Time.get_ticks_msec()
	combine.emit(npc_id, selected, _pending_request)


func _process(_delta: float) -> void:
	# A combine the server refused answers only in chat: stop waiting for a result that is not coming.
	if _waiting and Time.get_ticks_msec() - _sent_at_ms > 4000:
		_waiting = false
		repeat_left = 0
	if repeat_left > 0 and not _waiting and _next_at_ms > 0 and Time.get_ticks_msec() >= _next_at_ms:
		_next_at_ms = 0
		repeat_left -= 1
		_send()


# ------------------------------------------------------------------------------ drawing

func select(id: String) -> void:
	if id != selected:
		repeat_left = 0  # a run of combines is for the recipe it started with
		_next_at_ms = 0
	selected = id
	_refresh_detail()


func _refresh() -> void:
	var ts: Dictionary = tradeskill_def(tradeskill)
	_title.text = "%s - %s" % [station_name, str(ts.get("name", tradeskill))]
	var cap: int = int((skills.get(tradeskill, {}) as Dictionary).get("cap", (data().get("rules", {}) as Dictionary).get("max_skill", 300)))
	_skill_label.text = "Your %s: %d / %d" % [str(ts.get("name", tradeskill)), skill_in(tradeskill), cap]
	for c: Node in _list.get_children():
		c.queue_free()
	var shown: Array = visible_recipes()
	var skill: int = skill_in(tradeskill)
	for i: int in range(mini(shown.size(), MAX_ROWS)):
		var r: Dictionary = shown[i]
		var id: String = str(r["id"])
		var res: Dictionary = r["result"]
		var b: Button = Button.new()
		b.flat = true
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		var n: int = int(res["count"])
		b.text = "%s%s  [%d]" % [ItemDb.name_of(str(res["item"])), (" x%d" % n) if n > 1 else "", int(r["trivial"])]
		var col: Color = DIFFICULTY_COLORS[difficulty(skill, int(r["trivial"]))]
		if not can_make(r):
			col = col.darkened(0.45)
		b.add_theme_color_override("font_color", col)
		b.add_theme_color_override("font_hover_color", col.lightened(0.2))
		b.set_meta("recipe", id)
		b.pressed.connect(func() -> void: select(id))
		_list.add_child(b)
	if shown.size() > MAX_ROWS:
		UI.label(_list, "%d more: search to narrow the list." % (shown.size() - MAX_ROWS))
	elif shown.is_empty():
		UI.label(_list, "Nothing matches.")
	_refresh_detail()


func _refresh_detail() -> void:
	_detail.text = describe(selected)
	var r: Dictionary = recipe(selected)
	_combine_btn.disabled = r.is_empty() or not can_make(r)


func _build() -> void:
	_win = PanelContainer.new()
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = Color(0.05, 0.04, 0.04, 0.9)
	s.border_color = Color(0.4, 0.3, 0.22)
	s.set_border_width_all(1)
	s.set_corner_radius_all(4)
	s.set_content_margin_all(8)
	_win.add_theme_stylebox_override("panel", s)
	_win.position = Vector2(40, 80)
	_win.custom_minimum_size = Vector2(620, 390)
	_win.visible = false
	add_child(_win)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	_win.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	_title = Label.new()
	_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_title.add_theme_color_override("font_color", UI.ACCENT)
	head.add_child(_title)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(close_windows)
	head.add_child(close)
	_skill_label = UI.label(box, "")
	var filters: HBoxContainer = HBoxContainer.new()
	box.add_child(filters)
	_search = LineEdit.new()
	_search.placeholder_text = "Search recipes"
	_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search.text_changed.connect(func(t: String) -> void:
		search_text = t
		_refresh())
	filters.add_child(_search)
	_makeable_box = CheckBox.new()
	_makeable_box.text = "Can make now"
	_makeable_box.toggled.connect(func(on: bool) -> void:
		only_makeable = on
		_refresh())
	filters.add_child(_makeable_box)
	_trivial_box = CheckBox.new()
	_trivial_box.text = "Hide trivial"
	_trivial_box.toggled.connect(func(on: bool) -> void:
		hide_trivial = on
		_refresh())
	filters.add_child(_trivial_box)
	var body: HBoxContainer = HBoxContainer.new()
	body.add_theme_constant_override("separation", 8)
	box.add_child(body)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(300, 280)
	body.add_child(scroll)
	_list = VBoxContainer.new()
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_list)
	var right: VBoxContainer = VBoxContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body.add_child(right)
	_detail = Label.new()
	_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_detail.custom_minimum_size = Vector2(290, 220)
	_detail.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_detail)
	var row: HBoxContainer = HBoxContainer.new()
	right.add_child(row)
	_count = SpinBox.new()
	_count.min_value = 1
	_count.max_value = 20
	_count.value = 1
	_count.tooltip_text = "How many times to combine in a row"
	row.add_child(_count)
	_combine_btn = Button.new()
	_combine_btn.text = "Combine"
	_combine_btn.disabled = true
	_combine_btn.pressed.connect(func() -> void: start_combine(int(_count.value)))
	row.add_child(_combine_btn)
	_status = UI.label(right, "")
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
