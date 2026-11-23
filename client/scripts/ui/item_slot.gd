## One item square: a pack, bag, bank, worn or trade slot. Shows the item (rarity-coloured initials and
## a count), its tooltip with a comparison against what is worn, and supports drag-and-drop: drag an
## item onto another slot to move it (Shift-drag takes one from a stack); right-click to use it.
## Pure presentation: the owner (item_ui.gd) turns signals into server intents.
extends Panel

const ItemDb := preload("res://scripts/ui/item_db.gd")

## Something was dropped here. data = {kind: "item", code, item_id, count, source}.
signal dropped(data: Dictionary, target: Control)
## Right-clicked (or double-clicked) a filled slot.
signal used(target: Control)

var code: int = ItemDb.NO_SLOT  # the inventory slot this square shows (NO_SLOT for trade/display squares)
var role: String = "inv"        # inv, equip, trade, theirs, post, destroy, sell
var label_text: String = ""     # shown when empty (e.g. "Head")
var item_id: String = ""
var count: int = 0
var accept_drops: bool = true
var draggable: bool = true
var compare_with: Dictionary = {}  # the worn item for the tooltip comparison
var _name: Label
var _count: Label
const Icons := preload("res://scripts/ui/icons.gd")

var _style: StyleBoxFlat
var _icon: TextureRect


func _ready() -> void:
	custom_minimum_size = Vector2(46, 46)
	mouse_filter = Control.MOUSE_FILTER_STOP
	_style = StyleBoxFlat.new()
	_style.set_border_width_all(2)
	_style.set_corner_radius_all(3)
	add_theme_stylebox_override("panel", _style)
	_name = Label.new()
	_name.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_name.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_name.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_name.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_name.add_theme_font_size_override("font_size", 12)
	_name.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_name)
	_icon = Icons.make(self, 6.0)
	_count = Label.new()
	_count.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	_count.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	_count.grow_vertical = Control.GROW_DIRECTION_BEGIN
	_count.offset_right = -3
	_count.add_theme_font_size_override("font_size", 11)
	_count.add_theme_color_override("font_outline_color", Color.BLACK)
	_count.add_theme_constant_override("outline_size", 4)
	_count.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_count)
	_refresh()


func set_item(id: String, n: int, worn: Dictionary = {}) -> void:
	item_id = id
	count = n
	compare_with = worn
	_refresh()


func clear() -> void:
	set_item("", 0)


func _refresh() -> void:
	if _style == null:
		return
	var filled: bool = item_id != ""
	_style.bg_color = Color(0.12, 0.1, 0.09, 0.95) if filled else Color(0.06, 0.05, 0.05, 0.8)
	_style.border_color = ItemDb.rarity_color(item_id) if filled else Color(0.35, 0.28, 0.22)
	if role == "destroy":
		_style.bg_color = Color(0.3, 0.05, 0.05, 0.85)
	var icon: Array = Icons.for_item(ItemDb.item(item_id)) if filled else []
	Icons.show(_icon, icon)
	if filled:
		_name.text = ItemDb.short_name(item_id) if icon.is_empty() else ""
		_name.add_theme_color_override("font_color", ItemDb.rarity_color(item_id))
		_count.text = str(count) if count > 1 else ""
		tooltip_text = ItemDb.tooltip(item_id, count, compare_with)
	else:
		_name.text = label_text
		_name.add_theme_color_override("font_color", Color(0.5, 0.45, 0.4))
		_count.text = ""
		tooltip_text = label_text


func _gui_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb == null or not mb.pressed or item_id == "":
		return
	if mb.button_index == MOUSE_BUTTON_RIGHT or (mb.button_index == MOUSE_BUTTON_LEFT and mb.double_click):
		used.emit(self)
		accept_event()


func _get_drag_data(_at_position: Vector2) -> Variant:
	if item_id == "" or not draggable:
		return null
	var n: int = 1 if Input.is_key_pressed(KEY_SHIFT) and count > 1 else 0  # 0 = the whole stack
	var preview: Label = Label.new()
	preview.text = ItemDb.name_of(item_id) + ((" x%d" % (count if n == 0 else 1)) if count > 1 else "")
	preview.add_theme_color_override("font_color", ItemDb.rarity_color(item_id))
	set_drag_preview(preview)
	return {"kind": "item", "code": code, "item_id": item_id, "count": n, "source": role}


func _can_drop_data(_at_position: Vector2, data: Variant) -> bool:
	return accept_drops and data is Dictionary and str((data as Dictionary).get("kind", "")) == "item"


func _drop_data(_at_position: Vector2, data: Variant) -> void:
	dropped.emit(data as Dictionary, self)
