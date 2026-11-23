## Full-screen cues for our own character (Phase 9): a red vignette that pulses at low health, an edge
## tint while stunned, mezzed, rooted or fleeing, and a gold flash on a level up. Colours, strengths
## and the low-health threshold come from effects.json "screen". It never takes mouse input.
extends Control

const Effects := preload("res://scripts/world/effects.gd")

var _edge: TextureRect
var _flash: ColorRect
var _hp_frac: float = 1.0
var _states: Dictionary = {}
var _time: float = 0.0
## The cue being drawn ("" = none), for tests.
var showing: String = ""


func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	var g: Gradient = Gradient.new()
	g.set_color(0, Color(1, 1, 1, 0))
	g.set_color(1, Color(1, 1, 1, 1))
	g.add_point(0.55, Color(1, 1, 1, 0))
	var tex: GradientTexture2D = GradientTexture2D.new()
	tex.gradient = g
	tex.fill = GradientTexture2D.FILL_RADIAL
	tex.fill_from = Vector2(0.5, 0.5)
	tex.fill_to = Vector2(1.0, 1.0)
	tex.width = 256
	tex.height = 256
	_edge = TextureRect.new()
	_edge.texture = tex
	_edge.stretch_mode = TextureRect.STRETCH_SCALE
	_edge.set_anchors_preset(Control.PRESET_FULL_RECT)
	_edge.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_edge.modulate = Color(1, 1, 1, 0)
	add_child(_edge)
	_flash = ColorRect.new()
	_flash.set_anchors_preset(Control.PRESET_FULL_RECT)
	_flash.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_flash.color = Color(1, 1, 1, 0)
	add_child(_flash)


func _cue(key: String) -> Dictionary:
	var v: Variant = (Effects.data().get("screen", {}) as Dictionary).get(key)
	return v if v is Dictionary else {}


## Our health and states this frame ({stunned, mezzed, rooted, fleeing: bool}).
func update_state(hp_frac: float, states: Dictionary) -> void:
	_hp_frac = hp_frac
	_states = states


func level_up() -> void:
	var c: Dictionary = _cue("level_up")
	if c.is_empty():
		return
	var col: Color = Color.html(str(c.get("color", "#ffd84a")))
	_flash.color = Color(col.r, col.g, col.b, float(c.get("strength", 0.5)))
	var tw: Tween = _flash.create_tween()
	tw.tween_property(_flash, "color:a", 0.0, float(c.get("duration", 1.4)))


func _process(delta: float) -> void:
	_time += delta
	showing = ""
	var col: Color = Color(1, 1, 1, 0)
	for key: String in ["mezzed", "stunned", "fleeing", "rooted"]:
		if bool(_states.get(key, false)) and not _cue(key).is_empty():
			var c: Dictionary = _cue(key)
			var base: Color = Color.html(str(c.get("color", "#ffffff")))
			col = Color(base.r, base.g, base.b, float(c.get("strength", 0.35)))
			showing = key
			break
	var low: Dictionary = _cue("low_health")
	if showing == "" and not low.is_empty() and _hp_frac > 0.0 and _hp_frac <= float(low.get("threshold", 0.25)):
		var base: Color = Color.html(str(low.get("color", "#ff2a1f")))
		var pulse: float = 0.75 + 0.25 * sin(_time * 5.0)
		col = Color(base.r, base.g, base.b, float(low.get("strength", 0.55)) * pulse)
		showing = "low_health"
	_edge.modulate = _edge.modulate.lerp(col, clampf(delta * 8.0, 0.0, 1.0))
