## Full-screen loading overlay (Phase 8), shown while a world is being entered or a zone line crossed.
## Sits on its own canvas layer above everything; main.gd shows and hides it.
extends CanvasLayer

const UI := preload("res://scripts/ui/ui.gd")
const TIPS: Array[String] = [
	"Sit (X) between fights: you regain health and mana far faster.",
	"Consider (C) a foe before you fight it. Red means run.",
	"Your corpse keeps your gear for an hour. Only you can loot it.",
	"Grouped, everyone near the kill shares the experience, with a bonus for each member.",
	"Tab cycles targets; F1 targets yourself, F2 to F6 your group.",
	"Spells are learned from scrolls and memorized into your gem slots.",
	"Bind yourself near a city: it is where you return after a defeat.",
	"Keys can be rebound under Settings, Keys.",
]

var _title: Label
var _tip: Label
var _dots: Label
var _t: float = 0.0
var showing: bool = false


func _ready() -> void:
	layer = 100
	visible = false
	var root: Control = Control.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_STOP  # nothing underneath is clickable while loading
	add_child(root)
	var bg: ColorRect = ColorRect.new()
	bg.color = UI.BG_COLOR
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.add_child(bg)
	var center: CenterContainer = CenterContainer.new()
	center.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.add_child(center)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 14)
	box.custom_minimum_size = Vector2(520, 0)
	center.add_child(box)
	_title = UI.title(box, "Entering the world", 30)
	_dots = UI.label(box, "")
	_dots.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_tip = UI.label(box, "")
	_tip.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER


func show_loading(zone_name: String) -> void:
	set_zone(zone_name)
	_tip.text = "Tip: " + TIPS[randi() % TIPS.size()]
	visible = true
	showing = true


func set_zone(zone_name: String) -> void:
	_title.text = "Entering %s" % zone_name if zone_name != "" else "Entering the world"


func hide_loading() -> void:
	visible = false
	showing = false


func _process(delta: float) -> void:
	if not visible:
		return
	_t += delta
	_dots.text = "Loading" + ".".repeat(1 + int(_t * 2.0) % 3)
