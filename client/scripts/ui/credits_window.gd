## The Credits screen (Phase 9): every asset in the build with its author, source and licence, the
## Polyfork models one credit line each, and the full licence texts, all from res://data/credits.json,
## which tools/sync_client_content.py builds from ASSETS.md, the packs' LICENSE.txt files and
## LICENSES/. A full-screen overlay like the settings window; Close or Esc returns.
extends Control

const DATA_PATH: String = "res://data/credits.json"

var _list: VBoxContainer
var _text: RichTextLabel
var _back: Button
var _scroll: ScrollContainer
var _list_scroll: int = 0


static func data() -> Dictionary:
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(DATA_PATH))
	return parsed if parsed is Dictionary else {}


func _ready() -> void:
	# Anchors and offsets both: an overlay must fill the screen it opens over (as the settings window).
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_STOP
	var dim: ColorRect = ColorRect.new()
	dim.color = Color(0, 0, 0, 0.6)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(dim)
	var panel: PanelContainer = PanelContainer.new()
	panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	panel.offset_left = 80
	panel.offset_right = -80
	panel.offset_top = 40
	panel.offset_bottom = -40
	var opaque: StyleBoxFlat = (get_theme_stylebox("panel", "PanelContainer") as StyleBoxFlat).duplicate() if get_theme_stylebox("panel", "PanelContainer") is StyleBoxFlat else StyleBoxFlat.new()
	opaque.bg_color = Color(opaque.bg_color, 1.0)  # long text reads badly over the screen behind
	panel.add_theme_stylebox_override("panel", opaque)
	add_child(panel)
	var margin: MarginContainer = MarginContainer.new()
	for side: String in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 14)
	panel.add_child(margin)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	margin.add_child(box)
	var head: HBoxContainer = HBoxContainer.new()
	box.add_child(head)
	var title: Label = Label.new()
	title.text = "Credits"
	title.add_theme_font_size_override("font_size", 24)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(title)
	_back = Button.new()
	_back.text = "Back to credits"
	_back.visible = false
	_back.pressed.connect(_show_list)
	head.add_child(_back)
	var close: Button = Button.new()
	close.text = "Close"
	close.pressed.connect(func() -> void: visible = false)
	head.add_child(close)
	var scroll: ScrollContainer = ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	box.add_child(scroll)
	_scroll = scroll
	var inner: VBoxContainer = VBoxContainer.new()
	inner.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(inner)
	_list = VBoxContainer.new()
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.add_theme_constant_override("separation", 6)
	inner.add_child(_list)
	_text = RichTextLabel.new()
	_text.fit_content = true
	_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_text.visible = false
	inner.add_child(_text)
	_fill()


func _unhandled_input(event: InputEvent) -> void:
	if visible and event is InputEventKey and (event as InputEventKey).pressed and (event as InputEventKey).keycode == KEY_ESCAPE:
		visible = false
		get_viewport().set_input_as_handled()


func _heading(text: String) -> void:
	var l: Label = Label.new()
	l.text = text
	l.add_theme_font_size_override("font_size", 18)
	l.add_theme_color_override("font_color", Color("#e0b850"))
	_list.add_child(l)


func _line(text: String, size: int = 13, color: Color = Color("#efe6d2")) -> void:
	var l: Label = Label.new()
	l.text = text
	l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	l.add_theme_font_size_override("font_size", size)
	l.add_theme_color_override("font_color", color)
	_list.add_child(l)


func _fill() -> void:
	var d: Dictionary = data()
	_heading("Realms of Emberfall")
	_line(str(d.get("intro", "")))
	_heading("Art, music and sound")
	for a: Variant in d.get("assets", []):
		var e: Dictionary = a as Dictionary
		_line("%s" % str(e.get("asset", "")), 13)
		_line("    by %s   |   %s   |   %s" % [str(e.get("author", "")), str(e.get("license", "")), str(e.get("source", ""))], 11, Color("#a99f8c"))
	var lines: Array = d.get("model_credits", []) as Array
	if not lines.is_empty():
		_heading("Polyfork models (%d)" % lines.size())
		_line("\n".join(lines.map(func(x: Variant) -> String: return str(x))), 11, Color("#c9bfaa"))
	_heading("Licences")
	for name: Variant in (d.get("licenses", {}) as Dictionary).keys():
		var b: Button = Button.new()
		b.text = str(name)
		b.alignment = HORIZONTAL_ALIGNMENT_LEFT
		var n: String = str(name)
		b.pressed.connect(func() -> void: _show_license(n))
		_list.add_child(b)


func _show_license(name: String) -> void:
	_text.text = str((data().get("licenses", {}) as Dictionary).get(name, ""))
	_list_scroll = _scroll.scroll_vertical
	_scroll.scroll_vertical = 0
	_list.visible = false
	_text.visible = true
	_back.visible = true


func _show_list() -> void:
	_list.visible = true
	_text.visible = false
	_back.visible = false
	_restore_scroll.call_deferred()


func _restore_scroll() -> void:
	_scroll.scroll_vertical = _list_scroll  # after the list is laid out again
