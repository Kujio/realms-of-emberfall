## The game's UI theme (Phase 9): one palette and one set of frames for every window, panel, button,
## field, bar, tab, list, tooltip and scrollbar, applied to the whole window at start-up (main.gd), so
## controls built in code look alike without per-widget styling. Frames are StyleBoxFlat (drawn
## borders and corners), which stay crisp at any size: the UI scales with the window (project.godot
## stretches canvas items from a 1280x720 base), so 1080p, 1440p and 4K all get the same layout.
extends RefCounted

# Palette: deep plum-charcoal panels, bronze frames, gold accents, ivory text.
const BG: Color = Color("#17121b")
const BG_SOFT: Color = Color("#221a28")
const FRAME: Color = Color("#7a6440")
const FRAME_LIT: Color = Color("#c9a45a")
const GOLD: Color = Color("#e0b850")
const TEXT: Color = Color("#efe6d2")
const MUTED: Color = Color("#a99f8c")
const DISABLED: Color = Color("#6b6358")
## Rarity frame colours (item borders), shared with item tooltips.
const RARITY: Dictionary = {
	"common": Color("#d8d2c4"), "uncommon": Color("#5fd36a"), "rare": Color("#5c9cff"), "epic": Color("#c07cff"),
}


static func _box(bg: Color, border: Color, width: int, radius: int, pad: float = 6.0) -> StyleBoxFlat:
	var s: StyleBoxFlat = StyleBoxFlat.new()
	s.bg_color = bg
	s.border_color = border
	s.set_border_width_all(width)
	s.set_corner_radius_all(radius)
	s.set_content_margin_all(pad)
	s.anti_aliasing = true
	return s


static func build() -> Theme:
	var t: Theme = Theme.new()
	t.default_font_size = 14
	# Panels and windows: a dark field in a bronze frame, with a soft shadow.
	var panel: StyleBoxFlat = _box(Color(BG, 0.94), FRAME, 2, 6, 8.0)
	panel.shadow_color = Color(0, 0, 0, 0.45)
	panel.shadow_size = 6
	for type: String in ["PanelContainer", "Panel"]:
		t.set_stylebox("panel", type, panel)
	t.set_stylebox("panel", "PopupPanel", panel)
	t.set_stylebox("panel", "PopupMenu", panel)
	var win: StyleBoxFlat = panel.duplicate() as StyleBoxFlat
	win.expand_margin_top = 26
	win.border_width_top = 26
	win.border_color = FRAME
	t.set_stylebox("embedded_border", "Window", win)
	t.set_stylebox("embedded_unfocused_border", "Window", win)
	t.set_color("title_color", "Window", GOLD)
	t.set_stylebox("panel", "AcceptDialog", panel)
	t.set_stylebox("panel", "TooltipPanel", _box(Color(BG_SOFT, 0.97), FRAME_LIT, 1, 4, 7.0))
	t.set_color("font_color", "TooltipLabel", TEXT)
	# Text.
	for type: String in ["Label", "RichTextLabel", "CheckBox", "CheckButton", "LinkButton"]:
		t.set_color("font_color", type, TEXT)
	t.set_color("default_color", "RichTextLabel", TEXT)
	t.set_color("font_outline_color", "Label", Color(0, 0, 0, 0.85))
	# Buttons: raised bronze-edged plates, gold when hovered, sunk when pressed.
	t.set_stylebox("normal", "Button", _box(BG_SOFT, FRAME, 2, 5, 6.0))
	t.set_stylebox("hover", "Button", _box(BG_SOFT.lightened(0.08), FRAME_LIT, 2, 5, 6.0))
	t.set_stylebox("pressed", "Button", _box(BG.darkened(0.2), GOLD, 2, 5, 6.0))
	t.set_stylebox("disabled", "Button", _box(Color(BG, 0.7), DISABLED, 1, 5, 6.0))
	t.set_stylebox("focus", "Button", _box(Color(0, 0, 0, 0), GOLD, 1, 5, 6.0))
	t.set_color("font_color", "Button", TEXT)
	t.set_color("font_hover_color", "Button", GOLD.lightened(0.25))
	t.set_color("font_pressed_color", "Button", GOLD)
	t.set_color("font_disabled_color", "Button", DISABLED)
	for state: String in ["normal", "hover", "pressed", "disabled", "focus"]:
		t.set_stylebox(state, "OptionButton", t.get_stylebox(state, "Button"))
	t.set_color("font_color", "OptionButton", TEXT)
	# Fields.
	var field: StyleBoxFlat = _box(Color("#0e0b10"), FRAME.darkened(0.2), 1, 4, 6.0)
	t.set_stylebox("normal", "LineEdit", field)
	t.set_stylebox("focus", "LineEdit", _box(Color(0, 0, 0, 0), GOLD, 1, 4, 6.0))
	t.set_stylebox("read_only", "LineEdit", field)
	t.set_color("font_color", "LineEdit", TEXT)
	t.set_color("font_placeholder_color", "LineEdit", MUTED)
	t.set_color("caret_color", "LineEdit", GOLD)
	t.set_stylebox("normal", "TextEdit", field)
	# Bars: dark troughs; each bar sets its own fill colour (health red, mana blue, ...).
	t.set_stylebox("background", "ProgressBar", _box(Color("#0b090d", 0.9), FRAME.darkened(0.3), 1, 3, 0.0))
	t.set_stylebox("fill", "ProgressBar", _box(GOLD.darkened(0.2), Color(0, 0, 0, 0), 0, 3, 0.0))
	t.set_color("font_color", "ProgressBar", TEXT)
	# Tabs.
	t.set_stylebox("tab_selected", "TabBar", _box(BG_SOFT, FRAME_LIT, 1, 4, 6.0))
	t.set_stylebox("tab_unselected", "TabBar", _box(Color(BG, 0.9), FRAME.darkened(0.3), 1, 4, 6.0))
	t.set_stylebox("tab_hovered", "TabBar", _box(BG_SOFT.lightened(0.05), FRAME_LIT, 1, 4, 6.0))
	t.set_color("font_selected_color", "TabBar", GOLD)
	t.set_color("font_unselected_color", "TabBar", MUTED)
	t.set_color("font_hovered_color", "TabBar", TEXT)
	for k: String in ["tab_selected", "tab_unselected", "tab_hovered"]:
		t.set_stylebox(k, "TabContainer", t.get_stylebox(k, "TabBar"))
	t.set_stylebox("panel", "TabContainer", panel)
	t.set_color("font_selected_color", "TabContainer", GOLD)
	t.set_color("font_unselected_color", "TabContainer", MUTED)
	# Lists and trees.
	t.set_stylebox("panel", "ItemList", field)
	t.set_stylebox("selected", "ItemList", _box(Color(GOLD, 0.25), Color(0, 0, 0, 0), 0, 3, 2.0))
	t.set_stylebox("selected_focus", "ItemList", _box(Color(GOLD, 0.3), GOLD, 1, 3, 2.0))
	t.set_color("font_color", "ItemList", TEXT)
	t.set_stylebox("panel", "Tree", field)
	# Scrollbars: slim bronze grabbers.
	for type: String in ["VScrollBar", "HScrollBar"]:
		var styles: Dictionary = {"scroll": Color(0, 0, 0, 0.25), "grabber": FRAME, "grabber_highlight": FRAME_LIT, "grabber_pressed": GOLD}
		for key: String in styles.keys():
			var b: StyleBoxFlat = _box(styles[key] as Color, Color(0, 0, 0, 0), 0, 4, 0.0)
			# The bar's thickness comes from these margins across it (8 px).
			if type == "VScrollBar":
				b.content_margin_left = 4.0
				b.content_margin_right = 4.0
			else:
				b.content_margin_top = 4.0
				b.content_margin_bottom = 4.0
			t.set_stylebox(key, type, b)
	# Sliders (the settings window).
	t.set_stylebox("slider", "HSlider", _box(Color("#0b090d"), FRAME.darkened(0.3), 1, 3, 0.0))
	t.set_stylebox("grabber_area", "HSlider", _box(FRAME, Color(0, 0, 0, 0), 0, 3, 0.0))
	t.set_stylebox("grabber_area_highlight", "HSlider", _box(FRAME_LIT, Color(0, 0, 0, 0), 0, 3, 0.0))
	return t
