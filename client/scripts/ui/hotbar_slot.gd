## One hotbar slot. Accepts abilities dragged from the ability window (or another slot),
## shows a cooldown sweep and countdown, and activates on click or its number key.
## Right-click clears the slot.
extends Panel

signal activated(index: int)
signal changed(index: int, ability_id: String)
## Dragged from another slot: swap the two in one step (one save).
signal swapped(from_index: int, to_index: int)

var index: int = 0
var ability_id: String = ""
var key_text: String = ""
var _armed: bool = false
var _name: Label
var _key: Label
var _cooldown: ColorRect
var _seconds: Label
var _style: StyleBoxFlat


const Icons := preload("res://scripts/ui/icons.gd")

var _icon: TextureRect


func _ready() -> void:
	custom_minimum_size = Vector2(58, 58)
	mouse_filter = Control.MOUSE_FILTER_STOP
	_style = StyleBoxFlat.new()
	_style.bg_color = Color(0.1, 0.08, 0.08, 0.85)
	_style.border_color = Color(0.45, 0.35, 0.25)
	_style.set_border_width_all(2)
	_style.set_corner_radius_all(4)
	add_theme_stylebox_override("panel", _style)
	_name = Label.new()
	_name.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_name.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_name.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_name.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_name.add_theme_font_size_override("font_size", 11)
	_name.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_name)
	_icon = Icons.make(self, 10.0)
	_cooldown = ColorRect.new()
	_cooldown.color = Color(0, 0, 0, 0.6)
	_cooldown.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_cooldown.visible = false
	add_child(_cooldown)
	_seconds = Label.new()
	_seconds.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_seconds.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_seconds.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_seconds.add_theme_font_size_override("font_size", 18)
	_seconds.add_theme_color_override("font_outline_color", Color.BLACK)
	_seconds.add_theme_constant_override("outline_size", 5)
	_seconds.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_seconds)
	_key = Label.new()
	_key.position = Vector2(3, 0)
	_key.text = key_text
	_key.add_theme_font_size_override("font_size", 11)
	_key.add_theme_color_override("font_color", Color(1.0, 0.85, 0.5))
	_key.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_key)


## title = short display name ("" for an empty slot); usable = known at this level.
func show_ability(id: String, title: String, usable: bool) -> void:
	ability_id = id
	_name.text = title
	_name.modulate = Color.WHITE if usable else Color(0.5, 0.5, 0.5)
	var icon: Array = Icons.for_spell(title) if title != "" else []
	Icons.show(_icon, icon, not usable)
	# With an icon the name becomes a small caption along the bottom.
	_name.vertical_alignment = VERTICAL_ALIGNMENT_BOTTOM if not icon.is_empty() else VERTICAL_ALIGNMENT_CENTER
	_name.add_theme_font_size_override("font_size", 9 if not icon.is_empty() else 11)
	_name.add_theme_color_override("font_outline_color", Color.BLACK)
	_name.add_theme_constant_override("outline_size", 4)
	tooltip_text = title


## remaining/total in seconds; 0 hides the overlay. `active` highlights a toggle (auto-attack).
func show_state(remaining: float, total: float, active: bool) -> void:
	_style.border_color = Color(1.0, 0.55, 0.2) if active else Color(0.45, 0.35, 0.25)
	if remaining <= 0.0 or total <= 0.0:
		_cooldown.visible = false
		_seconds.text = ""
		return
	_cooldown.visible = true
	var frac: float = clampf(remaining / total, 0.0, 1.0)
	_cooldown.position = Vector2(0, size.y * (1.0 - frac))
	_cooldown.size = Vector2(size.x, size.y * frac)
	_seconds.text = ("%d" % ceili(remaining)) if remaining >= 1.0 else ""


func _gui_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb == null:
		return
	if mb.button_index == MOUSE_BUTTON_LEFT:
		# Activate on release, so starting a drag never fires the ability.
		if mb.pressed:
			_armed = ability_id != ""
		elif _armed and Rect2(Vector2.ZERO, size).has_point(mb.position):
			_armed = false
			activated.emit(index)
		else:
			_armed = false
		accept_event()
	elif mb.button_index == MOUSE_BUTTON_RIGHT and mb.pressed:
		changed.emit(index, "")
		accept_event()


func _get_drag_data(_at_position: Vector2) -> Variant:
	_armed = false  # a drag is not a click
	if ability_id == "":
		return null
	var preview: Label = Label.new()
	preview.text = _name.text
	set_drag_preview(preview)
	return {"ability": ability_id, "from_slot": index}


func _can_drop_data(_at_position: Vector2, data: Variant) -> bool:
	return data is Dictionary and (data as Dictionary).has("ability")


func _drop_data(_at_position: Vector2, data: Variant) -> void:
	var d: Dictionary = data
	var from: int = int(d.get("from_slot", -1))
	if from >= 0:
		if from != index:
			swapped.emit(from, index)
		return
	changed.emit(index, str(d["ability"]))
