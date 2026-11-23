## A spell in the spellbook. Drag it onto a gem (or double-click / press Mem) to memorize it.
extends PanelContainer

signal memorize_requested(spell_id: String)

const GemSlot := preload("res://scripts/ui/gem_slot.gd")
const TARGETS: Array[String] = ["enemy", "self", "friendly", "group", "area", "corpse"]
const RESISTS: Array[String] = ["", "magic", "fire", "cold", "poison", "disease"]

const Icons := preload("res://scripts/ui/icons.gd")

var spell: Dictionary = {}
var usable: bool = true


func setup(info: Dictionary, can_use: bool, gem_text: String) -> void:
	spell = info
	usable = can_use
	mouse_filter = Control.MOUSE_FILTER_STOP
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.1, 0.09, 0.13, 0.9) if usable else Color(0.07, 0.07, 0.07, 0.7)
	style.set_content_margin_all(4)
	add_theme_stylebox_override("panel", style)
	var outer: HBoxContainer = HBoxContainer.new()
	outer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(outer)
	var pic_box: Control = Control.new()
	pic_box.custom_minimum_size = Vector2(34, 34)
	pic_box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	outer.add_child(pic_box)
	Icons.show(Icons.make(pic_box, 2.0), Icons.for_spell(str(info["name"]), int(info["resist"])), not can_use)
	var box: VBoxContainer = VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	outer.add_child(box)
	var school: int = int(info["resist"])
	var head: Label = Label.new()
	head.mouse_filter = Control.MOUSE_FILTER_IGNORE
	head.text = "L%d  %s%s" % [int(info["level"]), str(info["name"]), ("   [gem %s]" % gem_text) if gem_text != "" else ""]
	head.add_theme_color_override("font_color", GemSlot.SCHOOL_COLORS[clampi(school, 0, 5)].lightened(0.2) if usable else Color(0.5, 0.5, 0.5))
	box.add_child(head)
	var facts: Label = Label.new()
	facts.mouse_filter = Control.MOUSE_FILTER_IGNORE
	facts.add_theme_font_size_override("font_size", 11)
	facts.modulate = Color(0.8, 0.8, 0.85)
	var parts: Array[String] = ["%d mana" % int(info["mana"]), "%.1fs cast" % (float(info["cast_ms"]) / 1000.0)]
	if int(info["recast_ms"]) > 0:
		parts.append("%ss recast" % _fmt(float(info["recast_ms"]) / 1000.0))
	if int(info["duration_ms"]) > 0:
		parts.append("lasts %s" % _duration(float(info["duration_ms"]) / 1000.0))
	if float(info["range"]) > 0.0:
		parts.append("%dm" % int(info["range"]))
	parts.append(TARGETS[clampi(int(info["target"]), 0, TARGETS.size() - 1)])
	var resist_label: String = RESISTS[school] if school >= 0 and school < RESISTS.size() else ""
	if resist_label != "":
		parts.append(resist_label)
	facts.text = "%s  ·  %s" % [str(info["type"]), "  ·  ".join(parts)]
	box.add_child(facts)
	var desc: Label = Label.new()
	desc.mouse_filter = Control.MOUSE_FILTER_IGNORE
	desc.text = str(info["description"])
	desc.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	desc.add_theme_font_size_override("font_size", 12)
	desc.modulate = Color(0.85, 0.85, 0.85) if usable else Color(0.5, 0.5, 0.5)
	box.add_child(desc)
	var mem: Button = Button.new()
	mem.text = "Mem"
	mem.tooltip_text = "Memorize into the first free gem"
	mem.disabled = not usable
	mem.pressed.connect(func() -> void: memorize_requested.emit(str(spell["id"])))
	outer.add_child(mem)
	tooltip_text = "Drag onto a spell gem to memorize"


static func _fmt(s: float) -> String:
	return ("%d" % int(s)) if is_equal_approx(s, roundf(s)) else ("%.1f" % s)


static func _duration(s: float) -> String:
	if s >= 60.0:
		return "%d min" % int(roundf(s / 60.0))
	return "%ss" % _fmt(s)


func _gui_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb != null and mb.pressed and mb.double_click and mb.button_index == MOUSE_BUTTON_LEFT and usable:
		memorize_requested.emit(str(spell["id"]))
		accept_event()


func _get_drag_data(_at_position: Vector2) -> Variant:
	var preview: Label = Label.new()
	preview.text = str(spell["name"])
	set_drag_preview(preview)
	return {"spell": str(spell["id"])}
