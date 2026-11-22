## World HUD pieces (Phase 6): the travel window (teleport stones, ferries, soulbinders), NPC dialogue
## (quest offers, turn-ins and progress), the quest journal (U) with a factions tab, the quest tracker,
## the minimap, and the map (M: this zone and the world). Pure presentation: choices go out as signals
## and the world sends them to the server.
extends Control

signal travel_chosen(npc_id: int, dest: String)
signal dialog_chosen(npc_id: int, key: String)
signal quest_abandoned(quest_id: String)

const REGARDS: Array[String] = ["Ally", "Warmly", "Kindly", "Amiably", "Indifferent", "Apprehensive", "Dubious", "Threatening", "Scowls"]
const MINIMAP_SIZE: float = 180.0
const MINIMAP_RANGE: float = 90.0  # metres from centre to edge

var travel_npc: int = 0
var travel_options: Array = []
var dialog_npc: int = 0
var dialog: Dictionary = {}
var journal: Array = []
var completed_quests: int = 0
var factions: Array = []
## Set by the world: a Callable returning {"pos": Vector3, "yaw": float, "dots": [[Vector3, Color], ...],
## "lines": [Dictionary], "bounds": Rect2, "zone": String, "zone_id": String, "props": Array}.
var map_source: Callable

var _travel_win: PanelContainer
var _travel_title: Label
var _travel_text: Label
var _travel_list: VBoxContainer
var _dialog_win: PanelContainer
var _dialog_title: Label
var _dialog_text: Label
var _dialog_list: VBoxContainer
var _journal_win: PanelContainer
var _journal_list: VBoxContainer
var _faction_list: VBoxContainer
var _journal_tabs: TabContainer
var _tracker: VBoxContainer
var _minimap: Control
var _map_win: PanelContainer
var _map_view: Control
var _world_list: VBoxContainer
var _world: Dictionary = {}


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string("res://data/world.json")) if FileAccess.file_exists("res://data/world.json") else null
	if parsed is Dictionary:
		_world = parsed
	_build_travel_window()
	_build_dialog_window()
	_build_journal_window()
	_build_tracker()
	_build_minimap()
	_build_map_window()


## Anchors `c` to a corner or the centre and offsets its top-left corner from that anchor by (x, y);
## the control grows right and down to its minimum size.
static func place(c: Control, preset: Control.LayoutPreset, x: float, y: float) -> void:
	c.set_anchors_preset(preset)
	c.offset_left = x
	c.offset_top = y
	c.offset_right = x
	c.offset_bottom = y
	c.grow_horizontal = Control.GROW_DIRECTION_END
	c.grow_vertical = Control.GROW_DIRECTION_END


## True if every open window, the minimap and the tracker lie inside the viewport.
func windows_on_screen() -> bool:
	return offscreen().is_empty()


## The open windows (and minimap, tracker) that stick out of the viewport, with their rectangles.
func offscreen() -> PackedStringArray:
	var out: PackedStringArray = PackedStringArray()
	# The HUD fills the screen; measure against it (the viewport itself, when there is no parent control).
	var view: Rect2 = get_global_rect().grow(1.0) if get_parent() is Control else get_viewport_rect().grow(1.0)
	for c: Control in [_travel_win, _dialog_win, _journal_win, _map_win, _minimap, _tracker]:
		if c.is_visible_in_tree() and not view.encloses(c.get_global_rect()):
			out.append("%s %s in %s" % [c.name, str(c.get_global_rect()), str(view)])
	return out


static func coin_text(copper: int) -> String:
	if copper <= 0:
		return "free"
	var parts: PackedStringArray = PackedStringArray()
	var units: Array = [[1000, "p"], [100, "g"], [10, "s"], [1, "c"]]
	var left: int = copper
	for u: Array in units:
		var n: int = left / int(u[0])
		left -= n * int(u[0])
		if n > 0:
			parts.append("%d%s" % [n, str(u[1])])
	return " ".join(parts)


func _window(title_text: String, width: float) -> Array:
	var win: PanelContainer = PanelContainer.new()
	win.visible = false
	win.mouse_filter = Control.MOUSE_FILTER_STOP
	win.custom_minimum_size = Vector2(width, 0.0)
	add_child(win)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	win.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	var title: Label = Label.new()
	title.text = title_text
	title.add_theme_font_size_override("font_size", 18)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(title)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(func() -> void: win.visible = false)
	head.add_child(close)
	return [win, box, title]


# ------------------------------------------------------------------------------ travel

func _build_travel_window() -> void:
	var w: Array = _window("", 360.0)
	_travel_win = w[0]
	_travel_win.name = "TravelWindow"
	place(_travel_win, Control.PRESET_CENTER, -180.0, -160.0)
	var box: VBoxContainer = w[1]
	_travel_title = w[2]
	_travel_text = Label.new()
	_travel_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_travel_text.custom_minimum_size = Vector2(340.0, 0.0)
	box.add_child(_travel_text)
	_travel_list = VBoxContainer.new()
	box.add_child(_travel_list)


## A TravelOptions message: the NPC's name, greeting and destinations.
func show_travel(msg: Dictionary) -> void:
	travel_npc = int(msg["npc_id"])
	travel_options = msg["options"] as Array
	_travel_title.text = str(msg["title"])
	_travel_text.text = str(msg["text"])
	for c: Node in _travel_list.get_children():
		c.queue_free()
	for o: Variant in travel_options:
		var od: Dictionary = o as Dictionary
		var b: Button = Button.new()
		var label: String = str(od["name"])
		if str(od["id"]) != "bind":
			label += "  (%s%s)" % [coin_text(int(od["fee"])), ", sails in %ds" % (int(od["delay_ms"]) / 1000) if int(od["delay_ms"]) > 0 else ""]
		b.text = label
		var dest: String = str(od["id"])
		b.pressed.connect(func() -> void:
			travel_chosen.emit(travel_npc, dest)
			close_travel())
		_travel_list.add_child(b)
	_travel_win.visible = true


func close_travel() -> void:
	_travel_win.visible = false


func travel_visible() -> bool:
	return _travel_win.visible


# ------------------------------------------------------------------------------ dialogue

func _build_dialog_window() -> void:
	var w: Array = _window("", 420.0)
	_dialog_win = w[0]
	_dialog_win.name = "DialogWindow"
	place(_dialog_win, Control.PRESET_CENTER_LEFT, 40.0, -200.0)
	var box: VBoxContainer = w[1]
	_dialog_title = w[2]
	_dialog_text = Label.new()
	_dialog_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_dialog_text.custom_minimum_size = Vector2(400.0, 0.0)
	box.add_child(_dialog_text)
	box.add_child(HSeparator.new())
	_dialog_list = VBoxContainer.new()
	box.add_child(_dialog_list)


## An NpcDialog message: what the NPC says, and the options (quest offers, turn-ins, replies).
func show_dialog(msg: Dictionary) -> void:
	dialog = msg
	dialog_npc = int(msg["npc_id"])
	_dialog_title.text = str(msg["name"])
	_dialog_text.text = str(msg["text"])
	for c: Node in _dialog_list.get_children():
		c.queue_free()
	for o: Variant in msg["options"]:
		var od: Dictionary = o as Dictionary
		var b: Button = Button.new()
		b.text = str(od["label"])
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		var key: String = str(od["key"])
		b.pressed.connect(func() -> void: choose(key))
		_dialog_list.add_child(b)
	if (msg["options"] as Array).is_empty():
		var bye: Button = Button.new()
		bye.text = "Farewell."
		bye.pressed.connect(close_dialog)
		_dialog_list.add_child(bye)
	_dialog_win.visible = true


func choose(key: String) -> void:
	dialog_chosen.emit(dialog_npc, key)


func dialog_options() -> Array:
	return dialog.get("options", []) as Array


func close_dialog() -> void:
	_dialog_win.visible = false


func dialog_visible() -> bool:
	return _dialog_win.visible


# ------------------------------------------------------------------------------ journal, factions, tracker

func _build_journal_window() -> void:
	var w: Array = _window("Journal", 440.0)
	_journal_win = w[0]
	_journal_win.name = "JournalWindow"
	place(_journal_win, Control.PRESET_CENTER_RIGHT, -480.0, -220.0)
	var box: VBoxContainer = w[1]
	_journal_tabs = TabContainer.new()
	_journal_tabs.custom_minimum_size = Vector2(420.0, 360.0)
	box.add_child(_journal_tabs)
	var qs: ScrollContainer = ScrollContainer.new()
	qs.name = "Quests"
	_journal_tabs.add_child(qs)
	_journal_list = VBoxContainer.new()
	_journal_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	qs.add_child(_journal_list)
	var fs: ScrollContainer = ScrollContainer.new()
	fs.name = "Factions"
	_journal_tabs.add_child(fs)
	_faction_list = VBoxContainer.new()
	_faction_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	fs.add_child(_faction_list)


func _build_tracker() -> void:
	_tracker = VBoxContainer.new()
	_tracker.name = "QuestTracker"
	_tracker.mouse_filter = Control.MOUSE_FILTER_IGNORE
	place(_tracker, Control.PRESET_TOP_RIGHT, -300.0, 230.0)
	_tracker.custom_minimum_size = Vector2(280.0, 0.0)
	add_child(_tracker)


static func quest_line(q: Dictionary) -> String:
	var progress: String = ""
	if int(q["need"]) > 0 and not bool(q["ready"]):
		progress = " (%d/%d)" % [int(q["have"]), int(q["need"])]
	return str(q["text"]) + progress


## A QuestJournal message: the active quests, newest last.
func set_journal(msg: Dictionary) -> void:
	journal = msg["quests"] as Array
	completed_quests = int(msg["completed"])
	for c: Node in _journal_list.get_children():
		c.queue_free()
	var head: Label = Label.new()
	head.text = "%d active, %d completed" % [journal.size(), completed_quests]
	_journal_list.add_child(head)
	for q: Variant in journal:
		var qd: Dictionary = q as Dictionary
		var row: VBoxContainer = VBoxContainer.new()
		var title: Label = Label.new()
		title.text = "%s%s  (level %d%s)" % ["[Epic] " if bool(qd["epic"]) else "", str(qd["name"]), int(qd["level"]),
			", " + str(qd["zone"]) if str(qd["zone"]) != "" else ""]
		title.add_theme_color_override("font_color", Color(1.0, 0.85, 0.4) if bool(qd["ready"]) else Color(0.9, 0.9, 0.85))
		row.add_child(title)
		var step: Label = Label.new()
		step.text = "  Step %d of %d: %s" % [mini(int(qd["step"]) + 1, int(qd["steps"])), int(qd["steps"]), quest_line(qd)]
		step.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		row.add_child(step)
		if str(qd["turn_in"]) != "":
			var who: Label = Label.new()
			who.text = "  See: " + str(qd["turn_in"])
			row.add_child(who)
		var ab: Button = Button.new()
		ab.text = "Abandon"
		var qid: String = str(qd["id"])
		ab.pressed.connect(func() -> void: quest_abandoned.emit(qid))
		row.add_child(ab)
		_journal_list.add_child(row)
	_refresh_tracker()


func _refresh_tracker() -> void:
	for c: Node in _tracker.get_children():
		c.queue_free()
	var shown: int = 0
	for q: Variant in journal:
		if shown >= 5:
			break
		var qd: Dictionary = q as Dictionary
		var l: Label = Label.new()
		l.text = "%s\n  %s" % [str(qd["name"]), quest_line(qd)]
		l.add_theme_font_size_override("font_size", 13)
		l.add_theme_color_override("font_color", Color(1.0, 0.85, 0.4) if bool(qd["ready"]) else Color(0.92, 0.92, 0.88))
		l.add_theme_color_override("font_outline_color", Color(0, 0, 0))
		l.add_theme_constant_override("outline_size", 3)
		_tracker.add_child(l)
		shown += 1


## A FactionStandings message.
func set_factions(list: Array) -> void:
	factions = list
	for c: Node in _faction_list.get_children():
		c.queue_free()
	if list.is_empty():
		var none: Label = Label.new()
		none.text = "No faction has taken notice of you yet."
		_faction_list.add_child(none)
	for f: Variant in list:
		var fd: Dictionary = f as Dictionary
		var l: Label = Label.new()
		var tier: int = int(fd["regard"])
		l.text = "%s: %s (%d)" % [str(fd["name"]), REGARDS[clampi(tier, 0, REGARDS.size() - 1)], int(fd["standing"])]
		l.add_theme_color_override("font_color", Color(0.6, 1.0, 0.6) if tier <= 3 else (Color(1.0, 0.5, 0.45) if tier >= 6 else Color(0.9, 0.9, 0.85)))
		_faction_list.add_child(l)


func toggle_journal() -> void:
	_journal_win.visible = not _journal_win.visible


func journal_visible() -> bool:
	return _journal_win.visible


# ------------------------------------------------------------------------------ minimap and map

## Draws what map_source reports: a top-down view around the player (minimap) or of the whole zone.
class MapView extends Control:
	var source: Callable
	var whole_zone: bool = false
	var range_m: float = 90.0

	func _process(_d: float) -> void:
		if is_visible_in_tree():
			queue_redraw()

	func _draw() -> void:
		var s: Vector2 = size
		draw_rect(Rect2(Vector2.ZERO, s), Color(0.06, 0.07, 0.06, 0.75))
		if not source.is_valid():
			return
		var m: Dictionary = source.call()
		if m.is_empty():
			return
		var pos: Vector3 = m["pos"]
		var b: Rect2 = m["bounds"]
		var centre: Vector2 = Vector2(pos.x, pos.z)
		var scale_k: float = (s.x * 0.5) / range_m
		if whole_zone:
			centre = b.get_center()
			scale_k = minf(s.x / maxf(1.0, b.size.x), s.y / maxf(1.0, b.size.y)) * 0.95
		var to_px: Callable = func(wx: float, wz: float) -> Vector2:
			return s * 0.5 + (Vector2(wx, wz) - centre) * scale_k
		# The zone's edge.
		var tl: Vector2 = to_px.call(b.position.x, b.position.y)
		var br: Vector2 = to_px.call(b.end.x, b.end.y)
		draw_rect(Rect2(tl, br - tl), Color(0.5, 0.45, 0.35, 0.6), false, 1.0)
		for w: Variant in m.get("walls", []):
			var wb: PackedFloat32Array = w
			var p: Vector2 = to_px.call(wb[0], wb[1])
			var hs: Vector2 = Vector2(wb[2], wb[3]) * scale_k
			draw_set_transform(p, -wb[4], Vector2.ONE)
			draw_rect(Rect2(-hs, hs * 2.0), Color(0.55, 0.5, 0.45, 0.8))
			draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)
		for o: Variant in m.get("circles", []):
			var ob: PackedFloat32Array = o
			draw_circle(to_px.call(ob[0], ob[1]), maxf(1.0, ob[2] * scale_k), Color(0.3, 0.42, 0.25, 0.8))
		var tiles: Dictionary = m.get("tiles", {})
		if not tiles.is_empty():
			var cell: float = float(tiles["cell"])
			var o2: Vector2 = tiles["origin"]
			var w: int = int(tiles["w"])
			var solid: PackedByteArray = tiles["solid"]
			for i: int in range(solid.size()):
				if solid[i] == 0:
					var x: float = o2.x + float(i % w) * cell
					var z: float = o2.y + float(i / w) * cell
					var a: Vector2 = to_px.call(x, z)
					draw_rect(Rect2(a, Vector2(cell, cell) * scale_k + Vector2(0.5, 0.5)), Color(0.35, 0.3, 0.26, 0.9))
		for l: Variant in m.get("lines", []):
			var ld: Dictionary = l
			var lp: Vector2 = to_px.call(float(ld["x"]), float(ld["z"]))
			var lh: Vector2 = Vector2(float(ld.get("hx", 4.0)), float(ld.get("hz", 4.0))) * scale_k
			draw_rect(Rect2(lp - lh, lh * 2.0), Color(0.4, 0.65, 1.0, 0.9))
			if whole_zone:
				draw_string(ThemeDB.fallback_font, lp + Vector2(6, -6), str(ld.get("label", "")), HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color(0.7, 0.85, 1.0))
		for d: Variant in m.get("dots", []):
			var da: Array = d
			var dp: Vector2 = to_px.call((da[0] as Vector3).x, (da[0] as Vector3).z)
			if Rect2(Vector2.ZERO, s).has_point(dp):
				draw_circle(dp, 3.0 if whole_zone else 2.5, da[1] as Color)
		# You: an arrow pointing where you face (yaw 0 faces -Z, up on the map).
		var me: Vector2 = to_px.call(pos.x, pos.z)
		var yaw: float = float(m["yaw"])
		var f: Vector2 = Vector2(-sin(yaw), -cos(yaw))
		var r: Vector2 = Vector2(f.y, -f.x)
		draw_colored_polygon(PackedVector2Array([me + f * 8.0, me - f * 4.0 + r * 5.0, me - f * 4.0 - r * 5.0]), Color(1.0, 0.9, 0.3))
		if not whole_zone:
			draw_string(ThemeDB.fallback_font, Vector2(6, 14), str(m["zone"]), HORIZONTAL_ALIGNMENT_LEFT, s.x - 12, 12, Color(0.95, 0.9, 0.75))


func _build_minimap() -> void:
	var view: MapView = MapView.new()
	view.name = "Minimap"
	view.mouse_filter = Control.MOUSE_FILTER_IGNORE
	view.custom_minimum_size = Vector2(MINIMAP_SIZE, MINIMAP_SIZE)
	place(view, Control.PRESET_TOP_RIGHT, -MINIMAP_SIZE - 12.0, 40.0)
	view.range_m = MINIMAP_RANGE
	view.clip_contents = true
	_minimap = view
	add_child(view)


func _build_map_window() -> void:
	var w: Array = _window("Map", 760.0)
	_map_win = w[0]
	_map_win.name = "MapWindow"
	place(_map_win, Control.PRESET_CENTER, -380.0, -290.0)
	var box: VBoxContainer = w[1]
	var tabs: TabContainer = TabContainer.new()
	tabs.custom_minimum_size = Vector2(740.0, 500.0)
	box.add_child(tabs)
	var view: MapView = MapView.new()
	view.name = "Zone"
	view.whole_zone = true
	view.clip_contents = true
	_map_view = view
	tabs.add_child(view)
	var ws: ScrollContainer = ScrollContainer.new()
	ws.name = "World"
	tabs.add_child(ws)
	_world_list = VBoxContainer.new()
	_world_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	ws.add_child(_world_list)


func set_map_source(src: Callable, zone_id: String) -> void:
	map_source = src
	(_minimap as MapView).source = src
	(_map_view as MapView).source = src
	_fill_world_list(zone_id)


## The world tab: every zone by level band with its kind and where its roads lead; yours highlighted.
func _fill_world_list(current: String) -> void:
	for c: Node in _world_list.get_children():
		c.queue_free()
	var zones: Array = _world.get("zones", []) as Array
	var names: Dictionary = {}
	for z: Variant in zones:
		names[str((z as Dictionary)["id"])] = str((z as Dictionary)["name"])
	var sorted: Array = zones.duplicate()
	sorted.sort_custom(func(a: Variant, b: Variant) -> bool:
		var la: int = int(((a as Dictionary).get("levels", {}) as Dictionary).get("min", 1))
		var lb: int = int(((b as Dictionary).get("levels", {}) as Dictionary).get("min", 1))
		return la < lb if la != lb else str((a as Dictionary)["name"]) < str((b as Dictionary)["name"]))
	for z: Variant in sorted:
		var zd: Dictionary = z as Dictionary
		if str(zd.get("kind", "")) == "test":
			continue
		var lv: Dictionary = zd.get("levels", {}) as Dictionary
		var links: PackedStringArray = PackedStringArray()
		for l: Variant in zd.get("links", []):
			links.append(str(names.get(str(l), str(l))))
		var label: Label = Label.new()
		label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		label.text = "%s%s  [%s, %d-%d]\n    to: %s" % ["> " if str(zd["id"]) == current else "", str(zd["name"]), str(zd.get("kind", "")),
			int(lv.get("min", 1)), int(lv.get("max", 60)), ", ".join(links)]
		if str(zd["id"]) == current:
			label.add_theme_color_override("font_color", Color(1.0, 0.85, 0.4))
		_world_list.add_child(label)


func toggle_map() -> void:
	_map_win.visible = not _map_win.visible


func map_visible() -> bool:
	return _map_win.visible


func any_window_open() -> bool:
	return _travel_win.visible or _dialog_win.visible or _journal_win.visible or _map_win.visible


func close_windows() -> void:
	_travel_win.visible = false
	_dialog_win.visible = false
	_journal_win.visible = false
	_map_win.visible = false
