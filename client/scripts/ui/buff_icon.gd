## One effect in a buff/debuff tray: initials on a school-colored square (blue rim for buffs,
## red for debuffs), the time left underneath, and a tooltip with the details.
extends VBoxContainer

const GemSlot := preload("res://scripts/ui/gem_slot.gd")

const Icons := preload("res://scripts/ui/icons.gd")

var spell_id: String = ""
var ends_msec: int = 0
var _box: PanelContainer
var _initials: Label
var _time: Label
var _style: StyleBoxFlat


func setup(info: Dictionary, description: String, received_msec: int) -> void:
	spell_id = str(info["spell_id"])
	ends_msec = received_msec + int(info["remaining_ms"])
	add_theme_constant_override("separation", 0)
	mouse_filter = Control.MOUSE_FILTER_PASS
	_box = PanelContainer.new()
	_box.custom_minimum_size = Vector2(30, 30)
	_style = StyleBoxFlat.new()
	var school: int = int(info["school"])
	_style.bg_color = GemSlot.SCHOOL_COLORS[clampi(school, 0, GemSlot.SCHOOL_COLORS.size() - 1)].darkened(0.55)
	_style.border_color = Color(0.4, 0.65, 1.0) if bool(info["beneficial"]) else Color(1.0, 0.35, 0.3)
	_style.set_border_width_all(2)
	_style.set_corner_radius_all(3)
	_box.add_theme_stylebox_override("panel", _style)
	_box.mouse_filter = Control.MOUSE_FILTER_PASS
	add_child(_box)
	var icon: Array = Icons.for_spell(str(info["name"]), school)
	var pic: TextureRect = Icons.make(_box, 2.0)
	Icons.show(pic, icon)
	_initials = Label.new()
	_initials.text = GemSlot.initials(str(info["name"])) if icon.is_empty() else ""
	_initials.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_initials.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_initials.add_theme_font_size_override("font_size", 11)
	_initials.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_box.add_child(_initials)
	_time = Label.new()
	_time.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_time.add_theme_font_size_override("font_size", 10)
	_time.add_theme_color_override("font_outline_color", Color.BLACK)
	_time.add_theme_constant_override("outline_size", 3)
	_time.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_time)
	var caster: String = str(info["caster"])
	tooltip_text = "%s%s\n%s%s" % [
		str(info["name"]), "" if bool(info["beneficial"]) else " (harmful)",
		("Cast by %s\n" % caster) if caster != "" else "", description]
	_box.tooltip_text = tooltip_text
	refresh()


func seconds_left() -> float:
	return maxf(0.0, float(ends_msec - Time.get_ticks_msec()) / 1000.0)


static func format_time(s: float) -> String:
	if s >= 3600.0:
		return "%dh" % int(s / 3600.0)
	if s >= 60.0:
		return "%dm" % int(ceilf(s / 60.0))
	return "%ds" % int(ceilf(s))


func refresh() -> void:
	var s: float = seconds_left()
	_time.text = format_time(s)
	# Blink for the last ten seconds so a fading buff is noticed.
	_box.modulate.a = 1.0 if s > 10.0 or int(Time.get_ticks_msec() / 400) % 2 == 0 else 0.45
