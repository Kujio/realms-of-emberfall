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
