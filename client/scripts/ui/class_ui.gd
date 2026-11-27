## Class-specific HUD pieces (Phase 4): the pet window (health, mode, target, and the attack /
## follow / guard / back off / dismiss commands), the Bard's song indicator with its pulse timer,
## and the skills window (J), which also lists the commands that can be dragged to the hotbar.
## Pure presentation: commands go out as signals and the world sends them to the server.
extends Control

signal pet_command(order: int)
signal hotbar_command(cmd: String)

const UI := preload("res://scripts/ui/ui.gd")

## PetCommand.command values (server/src/game/class_rules.hpp: PetOrder).
const PET_ATTACK: int = 0
const PET_FOLLOW: int = 1
const PET_GUARD: int = 2
const PET_BACK_OFF: int = 3
const PET_DISMISS: int = 4
const CMD_PREFIX: String = "cmd:"
## Hotbar commands: id -> label. The world runs them (see world.gd run_hotbar_command).
const COMMANDS: Array[Array] = [
	["cmd:sit", "Sit / Stand"], ["cmd:consider", "Consider"], ["cmd:loot", "Loot"], ["cmd:auto_fire", "Ranged attack"],
	["cmd:pet_attack", "Pet: Attack"], ["cmd:pet_follow", "Pet: Follow"], ["cmd:pet_guard", "Pet: Guard"],
	["cmd:pet_back", "Pet: Back off"], ["cmd:pet_dismiss", "Pet: Dismiss"], ["cmd:stop", "Stop cast / song"],
]

var pet_id: int = 0
var pet: Dictionary = {}
var song_id: String = ""
var song_name: String = ""
var skills: Array = []
var craft_skills: Array = []  # Phase 13: CraftSkills (every tradeskill)
var _pet_win: PanelContainer
var _pet_name: Label
var _pet_hp: ProgressBar
var _pet_state: Label
var _song_panel: PanelContainer
var _song_label: Label
var _song_bar: ProgressBar
var _song_pulse_ms: int = 0
var _song_started: int = 0
var _skills_win: PanelContainer
var _skills_list: VBoxContainer


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_song_indicator()
	_build_pet_window()
	_build_skills_window()


static func command_label(id: String) -> String:
	for c: Array in COMMANDS:
		if str(c[0]) == id:
			return str(c[1])
	return ""


static func is_command(id: String) -> bool:
	return command_label(id) != ""


# ------------------------------------------------------------------------------ construction

func _panel(alpha: float = 0.6) -> PanelContainer:
	var p: PanelContainer = PanelContainer.new()
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = Color(0.05, 0.04, 0.04, alpha)
	s.border_color = Color(0.4, 0.3, 0.22)
	s.set_border_width_all(1)
	s.set_corner_radius_all(4)
	s.set_content_margin_all(8)
	p.add_theme_stylebox_override("panel", s)
	add_child(p)
	return p


func _bar(parent: Control, fill: Color, height: float) -> ProgressBar:
	var b: ProgressBar = ProgressBar.new()
	b.custom_minimum_size = Vector2(0, height)
	b.show_percentage = false
	var bg: StyleBoxFlat = StyleBoxFlat.new()
	bg.bg_color = Color(0.12, 0.1, 0.1)
	var fg: StyleBoxFlat = StyleBoxFlat.new()
	fg.bg_color = fill
	b.add_theme_stylebox_override("background", bg)
	b.add_theme_stylebox_override("fill", fg)
	parent.add_child(b)
	return b


func _build_song_indicator() -> void:
	_song_panel = _panel()
	_song_panel.position = Vector2(12, 152)
	_song_panel.custom_minimum_size = Vector2(250, 0)
	var box: VBoxContainer = VBoxContainer.new()
	_song_panel.add_child(box)
	_song_label = Label.new()
	_song_label.add_theme_color_override("font_color", Color(0.75, 0.85, 1.0))
	box.add_child(_song_label)
	_song_bar = _bar(box, Color(0.45, 0.6, 1.0), 6)
	_song_panel.visible = false


func _build_pet_window() -> void:
	_pet_win = _panel()
	_pet_win.position = Vector2(12, 200)
	_pet_win.custom_minimum_size = Vector2(250, 0)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	_pet_win.add_child(box)
	_pet_name = Label.new()
	_pet_name.add_theme_color_override("font_color", Color(0.65, 1.0, 0.8))
	box.add_child(_pet_name)
	_pet_hp = _bar(box, Color(0.25, 0.7, 0.35), 12)
	_pet_state = Label.new()
	_pet_state.add_theme_font_size_override("font_size", 12)
	box.add_child(_pet_state)
	var row: HBoxContainer = HBoxContainer.new()
	row.add_theme_constant_override("separation", 3)
	box.add_child(row)
	for entry: Array in [["Attack", PET_ATTACK], ["Follow", PET_FOLLOW], ["Guard", PET_GUARD], ["Back", PET_BACK_OFF], ["Dismiss", PET_DISMISS]]:
		var b: Button = Button.new()
		b.text = str(entry[0])
		b.add_theme_font_size_override("font_size", 11)
		var order: int = int(entry[1])
		b.pressed.connect(func() -> void: pet_command.emit(order))
		row.add_child(b)
	_pet_win.visible = false


func _build_skills_window() -> void:
	_skills_win = _panel(0.88)
	_skills_win.position = Vector2(520, 90)
	_skills_win.custom_minimum_size = Vector2(320, 420)
	_skills_win.visible = false
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	_skills_win.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	var t: Label = Label.new()
	t.text = "Skills (J)"
	t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	t.add_theme_color_override("font_color", UI.ACCENT)
	head.add_child(t)
	var close: Button = Button.new()
	close.text = "x"
	close.pressed.connect(func() -> void: _skills_win.visible = false)
	head.add_child(close)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(310, 380)
	box.add_child(scroll)
	_skills_list = VBoxContainer.new()
	_skills_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_skills_list)


# ------------------------------------------------------------------------------ server state

## PetStatus: show, update, or hide the pet window.
func set_pet(msg: Dictionary) -> void:
	pet = msg
	pet_id = int(msg["pet_id"])
	_pet_win.visible = pet_id != 0
	if pet_id == 0:
		return
	_pet_name.text = "%s (level %d)%s" % [str(msg["name"]), int(msg["level"]), "  [charmed]" if bool(msg["charmed"]) else ""]
	_pet_hp.value = int(msg["hp_pct"])
	var mode: String = "Guarding" if int(msg["mode"]) == 1 else "Following"
	var doing: String = "attacking" if int(msg["target_id"]) != 0 else "idle"
	var charm: String = ""
	if bool(msg["charmed"]):
		charm = ", charm %ds" % int(ceilf(float(msg["charm_ms"]) / 1000.0))
	_pet_state.text = "%s, %s%s" % [mode, doing, charm]


## SongUpdate: the song the Bard is singing (an empty id hides the indicator).
func set_song(msg: Dictionary) -> void:
	song_id = str(msg["song_id"])
	song_name = str(msg["name"])
	_song_pulse_ms = int(msg["pulse_ms"])
	_song_started = Time.get_ticks_msec()
	_song_panel.visible = song_id != ""
	_song_label.text = "Singing: %s" % song_name


## SkillValues (the full list, not an announcement): refresh the skills window.
func set_skills(msg: Dictionary) -> void:
	if bool(msg["announce"]):
		for s: Dictionary in msg["skills"]:
			for i: int in range(skills.size()):
				if str((skills[i] as Dictionary)["id"]) == str(s["id"]):
					skills[i] = s
	else:
		skills = msg["skills"]
	if _skills_win.visible:
		_rebuild_skills()


## CraftSkills: the tradeskills, listed under the combat skills.
func set_craft_skills(msg: Dictionary) -> void:
	if bool(msg["announce"]):
		for s: Dictionary in msg["skills"]:
			for i: int in range(craft_skills.size()):
				if str((craft_skills[i] as Dictionary)["id"]) == str(s["id"]):
					craft_skills[i] = s
	else:
		craft_skills = msg["skills"]
	if _skills_win.visible:
		_rebuild_skills()


func toggle_skills() -> void:
	_skills_win.visible = not _skills_win.visible
	if _skills_win.visible:
		_rebuild_skills()


func any_window_open() -> bool:
	return _skills_win.visible


func close_windows() -> void:
	_skills_win.visible = false


func _rebuild_skills() -> void:
	for c: Node in _skills_list.get_children():
		c.queue_free()
	for s: Dictionary in skills:
		var row: HBoxContainer = HBoxContainer.new()
		var n: Label = Label.new()
		n.text = str(s["name"])
		n.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(n)
		var v: Label = Label.new()
		v.text = "%d / %d" % [int(s["value"]), int(s["cap"])]
		v.add_theme_color_override("font_color", Color(0.8, 0.9, 1.0) if int(s["value"]) < int(s["cap"]) else Color(1.0, 0.85, 0.4))
		row.add_child(v)
		_skills_list.add_child(row)
	if not craft_skills.is_empty():
		var th: Label = Label.new()
		th.text = "Tradeskills"
		th.add_theme_color_override("font_color", UI.ACCENT)
		_skills_list.add_child(th)
		for s: Dictionary in craft_skills:
			var row: HBoxContainer = HBoxContainer.new()
			var n: Label = Label.new()
			n.text = str(s["name"])
			n.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(n)
			var v: Label = Label.new()
			v.text = "%d / %d" % [int(s["value"]), int(s["cap"])]
			v.add_theme_color_override("font_color", Color(0.8, 0.9, 1.0) if int(s["value"]) < int(s["cap"]) else Color(1.0, 0.85, 0.4))
			row.add_child(v)
			_skills_list.add_child(row)
	var head: Label = Label.new()
	head.text = "Commands (drag to the hotbar)"
	head.add_theme_color_override("font_color", UI.ACCENT)
	_skills_list.add_child(head)
	for c: Array in COMMANDS:
		var b: Button = CommandButton.new()
		b.text = str(c[1])
		b.set("command_id", str(c[0]))
		var cid: String = str(c[0])
		b.pressed.connect(func() -> void: hotbar_command.emit(cid))
		_skills_list.add_child(b)


func _process(_delta: float) -> void:
	if _song_panel.visible and _song_pulse_ms > 0:
		var elapsed: int = (Time.get_ticks_msec() - _song_started) % _song_pulse_ms
		_song_bar.value = 100.0 * float(elapsed) / float(_song_pulse_ms)


## A command button that can be dragged onto a hotbar slot (the slot stores "cmd:...").
class CommandButton extends Button:
	var command_id: String = ""

	func _get_drag_data(_at_position: Vector2) -> Variant:
		var preview: Label = Label.new()
		preview.text = text
		set_drag_preview(preview)
		return {"ability": command_id}
