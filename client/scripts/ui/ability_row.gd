## A row in the ability window. Drag it onto a hotbar slot; double-click to use it.
extends PanelContainer

signal use_requested(ability_id: String)

const Icons := preload("res://scripts/ui/icons.gd")

var ability: Dictionary = {}
var known: bool = false


func setup(info: Dictionary, is_known: bool) -> void:
	ability = info
	known = is_known
	mouse_filter = Control.MOUSE_FILTER_STOP
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.12, 0.1, 0.1, 0.9) if is_known else Color(0.08, 0.08, 0.08, 0.7)
	style.set_content_margin_all(4)
	add_theme_stylebox_override("panel", style)
	var outer: HBoxContainer = HBoxContainer.new()
	outer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(outer)
	var pic_box: Control = Control.new()
	pic_box.custom_minimum_size = Vector2(34, 34)
	pic_box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	outer.add_child(pic_box)
	Icons.show(Icons.make(pic_box, 2.0), Icons.for_spell(str(info["name"])), not is_known)
	var box: VBoxContainer = VBoxContainer.new()
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	outer.add_child(box)
	var head: Label = Label.new()
	head.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var cost: String = ""
	if int(info["cost"]) > 0:
		cost = "  %d %s" % [int(info["cost"]), "end" if int(info["resource"]) == 1 else "mana"]
	var cd: float = float(info["cooldown_ms"]) / 1000.0
	head.text = "L%d  %s  [%s]%s%s" % [int(info["level"]), str(info["name"]), str(info["type"]), cost,
		("  %ss" % _fmt(cd)) if cd > 0.0 else ""]
	head.add_theme_color_override("font_color", Color(1.0, 0.85, 0.55) if is_known else Color(0.55, 0.55, 0.55))
	box.add_child(head)
	var desc: Label = Label.new()
	desc.mouse_filter = Control.MOUSE_FILTER_IGNORE
	desc.text = str(info["description"])
	desc.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	desc.add_theme_font_size_override("font_size", 12)
	desc.modulate = Color(0.85, 0.85, 0.85) if is_known else Color(0.5, 0.5, 0.5)
	box.add_child(desc)
	tooltip_text = "Drag to the hotbar" if str(info["type"]) != "passive" else "Passive: always active"


static func _fmt(s: float) -> String:
	return ("%d" % int(s)) if is_equal_approx(s, roundf(s)) else ("%.1f" % s)


func _gui_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb != null and mb.pressed and mb.double_click and mb.button_index == MOUSE_BUTTON_LEFT:
		use_requested.emit(str(ability["id"]))
		accept_event()


func _get_drag_data(_at_position: Vector2) -> Variant:
	if str(ability["type"]) == "passive":
		return null
	var preview: Label = Label.new()
	preview.text = str(ability["name"])
	set_drag_preview(preview)
	return {"ability": str(ability["id"])}
