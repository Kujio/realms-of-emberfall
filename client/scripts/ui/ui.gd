## Small helpers for building placeholder UI in code.
extends RefCounted

const BG_COLOR: Color = Color(0.07, 0.05, 0.06)
const ACCENT: Color = Color(0.91, 0.39, 0.17)
const ERROR_COLOR: Color = Color(1.0, 0.45, 0.4)
const OK_COLOR: Color = Color(0.85, 0.85, 0.8)


static func backdrop(parent: Control) -> void:
	parent.set_anchors_preset(Control.PRESET_FULL_RECT)
	var bg: ColorRect = ColorRect.new()
	bg.color = BG_COLOR
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(bg)


## Centered panel with a vertical box inside; returns the box.
static func centered_panel(parent: Control, min_width: float) -> VBoxContainer:
	var center: CenterContainer = CenterContainer.new()
	center.set_anchors_preset(Control.PRESET_FULL_RECT)
	parent.add_child(center)
	var panel: PanelContainer = PanelContainer.new()
	center.add_child(panel)
	var margin: MarginContainer = MarginContainer.new()
	for side: String in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 24)
	panel.add_child(margin)
	var box: VBoxContainer = VBoxContainer.new()
	box.custom_minimum_size = Vector2(min_width, 0)
	box.add_theme_constant_override("separation", 10)
	margin.add_child(box)
	return box


static func title(parent: Control, text: String, size: int) -> Label:
	var l: Label = Label.new()
	l.text = text
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_color", ACCENT)
	parent.add_child(l)
	return l


static func label(parent: Control, text: String) -> Label:
	var l: Label = Label.new()
	l.text = text
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	parent.add_child(l)
	return l


static func field(parent: Control, caption: String, value: String) -> LineEdit:
	label(parent, caption)
	var e: LineEdit = LineEdit.new()
	e.text = value
	e.custom_minimum_size = Vector2(0, 32)
	parent.add_child(e)
	return e


static func button(parent: Control, text: String, on_pressed: Callable) -> Button:
	var b: Button = Button.new()
	b.text = text
	b.custom_minimum_size = Vector2(120, 36)
	b.pressed.connect(func() -> void: sound("ui_click"))
	b.pressed.connect(on_pressed)
	parent.add_child(b)
	return b


static func row(parent: Control) -> HBoxContainer:
	var h: HBoxContainer = HBoxContainer.new()
	h.alignment = BoxContainer.ALIGNMENT_CENTER
	h.add_theme_constant_override("separation", 10)
	parent.add_child(h)
	return h


static func set_status(l: Label, text: String, is_error: bool) -> void:
	l.text = text
	l.add_theme_color_override("font_color", ERROR_COLOR if is_error else OK_COLOR)


## Put `side` to the right of the panel that centered_panel() built around `box`.
static func add_beside_panel(box: VBoxContainer, side: Control) -> void:
	var panel: Control = box.get_parent().get_parent() as Control
	var center: Control = panel.get_parent() as Control
	var h: HBoxContainer = HBoxContainer.new()
	h.add_theme_constant_override("separation", 16)
	center.remove_child(panel)
	center.add_child(h)
	h.add_child(panel)
	h.add_child(side)


## Plays a sound event through the Audio autoload (Phase 8), if it is running (the test runner has no
## autoloads). Events are listed in res://data/sounds.json.
static func sound(event: String, pitch_jitter: float = 0.0) -> void:
	var tree: SceneTree = Engine.get_main_loop() as SceneTree
	var audio: Node = tree.root.get_node_or_null("Audio") if tree != null else null
	if audio != null:
		audio.call("play", event, pitch_jitter)


## Starts (or stops) a looping sound event, such as the login ambience.
static func sound_loop(event: String, on: bool) -> void:
	var tree: SceneTree = Engine.get_main_loop() as SceneTree
	var audio: Node = tree.root.get_node_or_null("Audio") if tree != null else null
	if audio != null:
		audio.call("play_loop" if on else "stop_loop", event)


## What a player reads when the connection ends without them asking (an empty reason or a chosen logout
## shows no dialog).
static func disconnect_text(reason: String) -> String:
	var r: String = reason.strip_edges()
	if r == "" or r == "Logged out.":
		return ""
	if r == "Connection to the server was lost.":
		return "The connection to the server was lost.\n\nCheck your network connection and log in again. If you were in a fight, your character stays in the world for a short while, and logging back in quickly picks it up where it stands."
	if r == "Network error.":
		return "A network error ended the connection.\n\nLog in again; if it keeps happening, check your firewall and the server address."
	return r
