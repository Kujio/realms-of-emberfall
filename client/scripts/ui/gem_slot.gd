## One spell gem. Drop a spell from the spellbook here to memorize it; click to cast; right-click
## to forget; drag a gem onto the hotbar to bind it to a number key. Shows the memorize/recast
## sweep, and dims when the gem is still locked at this level.
extends Panel

const Icons := preload("res://scripts/ui/icons.gd")

signal cast_requested(gem: int)
signal memorize_requested(gem: int, spell_id: String)
signal forget_requested(gem: int)

const SCHOOL_COLORS: Array[Color] = [
	Color(0.85, 0.85, 0.7), Color(0.75, 0.5, 1.0), Color(1.0, 0.55, 0.2), Color(0.45, 0.85, 1.0),
	Color(0.55, 0.9, 0.3), Color(0.7, 0.55, 0.35),
]

var gem: int = 0
var spell_id: String = ""
var spell_name: String = ""
var unlocked: bool = true
var _armed: bool = false
var _label: Label
var _key: Label
var _sweep: ColorRect
var _style: StyleBoxFlat
var _icon: TextureRect


func _ready() -> void:
	custom_minimum_size = Vector2(46, 46)
	mouse_filter = Control.MOUSE_FILTER_STOP
	_style = StyleBoxFlat.new()
	_style.bg_color = Color(0.08, 0.07, 0.1, 0.85)
	_style.border_color = Color(0.35, 0.3, 0.5)
	_style.set_border_width_all(2)
	_style.set_corner_radius_all(23)
	add_theme_stylebox_override("panel", _style)
	_label = Label.new()
	_label.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_label.add_theme_font_size_override("font_size", 13)
	_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_label.add_theme_constant_override("outline_size", 4)
	_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_label)
	_icon = Icons.make(self, 9.0)
	_sweep = ColorRect.new()
	_sweep.color = Color(0, 0, 0, 0.6)
	_sweep.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_sweep.visible = false
	add_child(_sweep)
	_key = Label.new()
	_key.position = Vector2(2, -2)
	_key.text = str(gem + 1)
	_key.add_theme_font_size_override("font_size", 10)
	_key.add_theme_color_override("font_color", Color(0.75, 0.7, 1.0))
	_key.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_key)


## Two or three letters standing for a spell name ("Ember Dart" -> "ED").
static func initials(text: String) -> String:
	var out: String = ""
	for w: String in text.split(" ", false):
		if w.length() > 0 and out.length() < 3:
			out += w.substr(0, 1).to_upper()
	return out


func show_spell(id: String, title: String, school: int, is_unlocked: bool, description: String) -> void:
	spell_id = id
	spell_name = title
	unlocked = is_unlocked
	var icon: Array = Icons.for_spell(title, school) if id != "" else []
	Icons.show(_icon, icon, not is_unlocked)
	_label.text = (initials(title) if icon.is_empty() else "") if id != "" else ""
	var c: Color = SCHOOL_COLORS[clampi(school, 0, SCHOOL_COLORS.size() - 1)]
	_style.border_color = c if id != "" else (Color(0.35, 0.3, 0.5) if unlocked else Color(0.2, 0.2, 0.2))
	_style.bg_color = Color(0.08, 0.07, 0.1, 0.85) if unlocked else Color(0.03, 0.03, 0.03, 0.6)
	_label.add_theme_color_override("font_color", c.lightened(0.3))
	if not unlocked:
		tooltip_text = "Spell gem %d: locked until a higher level" % (gem + 1)
	elif id == "":
		tooltip_text = "Spell gem %d: empty. Drag a spell here from your spellbook (B)." % (gem + 1)
	else:
		tooltip_text = "%s (gem %d)\n%s\nClick to cast, right-click to forget, drag to the hotbar." % [title, gem + 1, description]


## remaining/total seconds of the memorize/recast lock (0 hides it); `busy` greys it while casting.
func show_state(remaining: float, total: float, busy: bool) -> void:
	modulate = Color(0.6, 0.6, 0.6) if busy and spell_id != "" else Color.WHITE
	if remaining <= 0.0 or total <= 0.0:
		_sweep.visible = false
		return
	_sweep.visible = true
	var frac: float = clampf(remaining / total, 0.0, 1.0)
	_sweep.position = Vector2(0, size.y * (1.0 - frac))
	_sweep.size = Vector2(size.x, size.y * frac)


func _gui_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb == null:
		return
	if mb.button_index == MOUSE_BUTTON_LEFT:
		if mb.pressed:
			_armed = spell_id != ""
		elif _armed and Rect2(Vector2.ZERO, size).has_point(mb.position):
			_armed = false
			cast_requested.emit(gem)
		else:
			_armed = false
		accept_event()
	elif mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
		if spell_id != "":
			forget_requested.emit(gem)
		accept_event()


func _get_drag_data(_at_position: Vector2) -> Variant:
	_armed = false
	if spell_id == "":
		return null
	var preview: Label = Label.new()
	preview.text = spell_name
	set_drag_preview(preview)
	return {"ability": "gem:%d" % gem}


func _can_drop_data(_at_position: Vector2, data: Variant) -> bool:
	return unlocked and data is Dictionary and (data as Dictionary).has("spell")


func _drop_data(_at_position: Vector2, data: Variant) -> void:
	memorize_requested.emit(gem, str((data as Dictionary)["spell"]))
