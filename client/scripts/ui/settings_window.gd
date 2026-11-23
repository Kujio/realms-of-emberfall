## The settings window (Phase 8): video, audio and key bindings. Opened from the login screen and the
## in-game menu; changes apply at once and are saved when the window closes.
extends Control

signal closed

const UI := preload("res://scripts/ui/ui.gd")

var _mode: OptionButton
var _res: OptionButton
var _vsync: CheckBox
var _sliders: Dictionary = {}
var _mute: CheckBox
var _bind_buttons: Dictionary = {}  # action -> Button
var _capturing: String = ""
var _hint: Label


## A full-screen overlay (it dims and blocks what is behind) with the window centered on it.
func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_STOP
	var dim: ColorRect = ColorRect.new()
	dim.color = Color(0, 0, 0, 0.45)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(dim)
	var center: CenterContainer = CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(center)
	var panel: PanelContainer = PanelContainer.new()
	panel.custom_minimum_size = Vector2(560, 480)
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.11, 0.09, 0.1, 0.98)
	style.set_corner_radius_all(4)
	panel.add_theme_stylebox_override("panel", style)
	center.add_child(panel)
	var margin: MarginContainer = MarginContainer.new()
	for side: String in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 14)
	panel.add_child(margin)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	margin.add_child(box)
	UI.title(box, "Settings", 24)
	var tabs: TabContainer = TabContainer.new()
	tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(tabs)
	tabs.add_child(_video_tab())
	tabs.add_child(_audio_tab())
	tabs.add_child(_keys_tab())
	var row: HBoxContainer = UI.row(box)
	UI.button(row, "Close", close)


## A one-line caption for a grid row (autowrapping labels collapse to nothing inside a GridContainer).
func _caption(parent: Control, text: String) -> Label:
	var l: Label = Label.new()
	l.text = text
	parent.add_child(l)
	return l


func _video_tab() -> Control:
	var g: GridContainer = GridContainer.new()
	g.name = "Video"
	g.columns = 2
	g.add_theme_constant_override("h_separation", 12)
	g.add_theme_constant_override("v_separation", 10)
	_caption(g, "Window")
	_mode = OptionButton.new()
	for m: String in ["Windowed", "Fullscreen (borderless)", "Fullscreen (exclusive)"]:
		_mode.add_item(m)
	_mode.selected = maxi(0, Settings.WINDOW_MODES.find(Settings.window_mode))
	_mode.item_selected.connect(func(i: int) -> void:
		Settings.window_mode = Settings.WINDOW_MODES[i]
		_res.disabled = i != 0
		Settings.apply_video())
	g.add_child(_mode)
	_caption(g, "Resolution (windowed)")
	_res = OptionButton.new()
	for r: Vector2i in Settings.RESOLUTIONS:
		_res.add_item("%d x %d" % [r.x, r.y])
	_res.selected = maxi(0, Settings.RESOLUTIONS.find(Settings.resolution))
	_res.disabled = Settings.window_mode != "windowed"
	_res.item_selected.connect(func(i: int) -> void:
		Settings.resolution = Settings.RESOLUTIONS[i]
		Settings.apply_video())
	g.add_child(_res)
	_caption(g, "Vertical sync")
	_vsync = CheckBox.new()
	_vsync.button_pressed = Settings.vsync
	_vsync.toggled.connect(func(on: bool) -> void:
		Settings.vsync = on
		Settings.apply_video())
	g.add_child(_vsync)
	return g


func _audio_tab() -> Control:
	var g: GridContainer = GridContainer.new()
	g.name = "Audio"
	g.columns = 3
	g.add_theme_constant_override("h_separation", 12)
	g.add_theme_constant_override("v_separation", 10)
	var names: Dictionary = {"Master": "Master volume", "Music": "Music", "Ambient": "Ambience", "SFX": "Effects", "UI": "Interface"}
	for bus: String in ["Master", "Music", "Ambient", "SFX", "UI"]:
		_caption(g, str(names[bus]))
		var s: HSlider = HSlider.new()
		s.min_value = 0.0
		s.max_value = 1.0
		s.step = 0.01
		s.value = float(Settings.volumes.get(bus, 0.8))
		s.custom_minimum_size = Vector2(240, 0)
		var pct: Label = Label.new()
		pct.text = "%d%%" % roundi(s.value * 100.0)
		s.value_changed.connect(func(v: float) -> void:
			Settings.volumes[bus] = v
			pct.text = "%d%%" % roundi(v * 100.0)
			Settings.apply_audio())
		s.drag_ended.connect(func(_c: bool) -> void: UI.sound("ui_click"))
		g.add_child(s)
		g.add_child(pct)
		_sliders[bus] = s
	_caption(g, "Mute everything")
	_mute = CheckBox.new()
	_mute.button_pressed = Settings.muted
	_mute.toggled.connect(func(on: bool) -> void:
		Settings.muted = on
		Settings.apply_audio())
	g.add_child(_mute)
	return g


func _keys_tab() -> Control:
	var outer: VBoxContainer = VBoxContainer.new()
	outer.name = "Keys"
	_hint = UI.label(outer, "Click a key, then press the new one (Esc cancels). A key moves from any action that had it.")
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.custom_minimum_size = Vector2(0, 300)
	outer.add_child(scroll)
	var g: GridContainer = GridContainer.new()
	g.columns = 2
	g.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	g.add_theme_constant_override("h_separation", 16)
	scroll.add_child(g)
	for action: String in Keybinds.action_ids():
		var l: Label = Label.new()
		l.text = Keybinds.label_of(action)
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		g.add_child(l)
		var b: Button = Button.new()
		b.custom_minimum_size = Vector2(150, 28)
		b.pressed.connect(func() -> void: _start_capture(action))
		g.add_child(b)
		_bind_buttons[action] = b
	var row: HBoxContainer = UI.row(outer)
	UI.button(row, "Reset to defaults", func() -> void:
		Settings.reset_binds()
		_refresh_binds())
	_refresh_binds()
	return outer


func _refresh_binds() -> void:
	for action: String in _bind_buttons.keys():
		var b: Button = _bind_buttons[action]
		b.text = "Press a key..." if action == _capturing else Keybinds.key_name(Settings.binds.get(action, []))


func _start_capture(action: String) -> void:
	_capturing = action
	_refresh_binds()


func is_capturing() -> bool:
	return _capturing != ""


func _input(event: InputEvent) -> void:
	if _capturing == "" or not visible:
		return
	var k: InputEventKey = event as InputEventKey
	if k == null or not k.pressed or k.echo:
		return
	get_viewport().set_input_as_handled()
	var key: int = int(k.physical_keycode) if k.physical_keycode != KEY_NONE else int(k.keycode)
	if key == KEY_ESCAPE:
		_capturing = ""
	elif Keybinds.RESERVED.has(key):
		_hint.text = "%s is reserved (chat and menus). Pick another key." % OS.get_keycode_string(key)
		return
	else:
		Settings.rebind(_capturing, key)
		_capturing = ""
	_refresh_binds()


func close() -> void:
	_capturing = ""
	Settings.save()
	UI.sound("ui_close")
	visible = false
	closed.emit()
