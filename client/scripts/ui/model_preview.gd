## A slowly turning 3D preview of a character for the select and create screens. show_character()
## rebuilds the model whenever race, gender or class changes, and plays a short flourish (a swing,
## or a cast for spellcasters) so the animation set is visible too.
extends SubViewportContainer

const CharacterVisual := preload("res://scripts/world/character_visual.gd")
const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const SPIN_SPEED: float = 0.6
const CASTERS: Array[String] = ["cleric", "druid", "shaman", "wizard", "magician", "necromancer", "enchanter"]

## What the preview currently shows ("" until the first show_character), for tests and the autotest.
var shown_key: String = ""
var visual: CharacterVisual

var _viewport: SubViewport
var _pivot: Node3D
var _flourish_in: float = 0.0
var _class_id: String = ""


func _init() -> void:
	stretch = true
	custom_minimum_size = Vector2(300, 400)
	_viewport = SubViewport.new()
	_viewport.own_world_3d = true
	_viewport.transparent_bg = true
	_viewport.msaa_3d = Viewport.MSAA_2X
	add_child(_viewport)
	var env: WorldEnvironment = WorldEnvironment.new()
	var e: Environment = Environment.new()
	e.background_mode = Environment.BG_CLEAR_COLOR
	e.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	e.ambient_light_color = Color(0.55, 0.5, 0.5)
	e.ambient_light_energy = 0.8
	env.environment = e
	_viewport.add_child(env)
	var sun: DirectionalLight3D = DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-40.0, 30.0, 0.0)
	sun.light_energy = 1.1
	_viewport.add_child(sun)
	var rim: DirectionalLight3D = DirectionalLight3D.new()
	rim.rotation_degrees = Vector3(-20.0, 200.0, 0.0)
	rim.light_energy = 0.5
	rim.light_color = Color(1.0, 0.7, 0.5)
	_viewport.add_child(rim)
	var cam: Camera3D = Camera3D.new()
	var eye: Vector3 = Vector3(0.0, 1.15, 3.4)
	cam.transform = Transform3D(Basis.looking_at(Vector3(0.0, 0.95, 0.0) - eye, Vector3.UP), eye)
	cam.fov = 38.0
	cam.current = true
	_viewport.add_child(cam)
	var floor_disc: MeshInstance3D = MeshInstance3D.new()
	var disc: CylinderMesh = CylinderMesh.new()
	disc.top_radius = 0.9
	disc.bottom_radius = 0.9
	disc.height = 0.04
	floor_disc.mesh = disc
	var fm: StandardMaterial3D = StandardMaterial3D.new()
	fm.albedo_color = Color(0.22, 0.16, 0.14)
	floor_disc.material_override = fm
	floor_disc.position.y = -0.02
	_viewport.add_child(floor_disc)
	_pivot = Node3D.new()
	_pivot.rotation.y = PI  # models face -Z; start turned toward the camera
	_viewport.add_child(_pivot)


func show_character(race_id: String, gender: int, class_id: String) -> void:
	var key: String = "%s/%d/%s" % [race_id, gender, class_id]
	if key == shown_key:
		return
	shown_key = key
	_class_id = class_id
	if visual != null:
		visual.queue_free()
	visual = CharacterVisual.new()
	visual.build(VisualLibrary.player_look(race_id, gender, class_id), 1.0)
	_pivot.add_child(visual)
	_pivot.rotation.y = PI
	_flourish_in = 0.6


func clear() -> void:
	shown_key = ""
	if visual != null:
		visual.queue_free()
		visual = null


func _process(delta: float) -> void:
	_pivot.rotation.y = wrapf(_pivot.rotation.y + delta * SPIN_SPEED, -PI, PI)
	if visual == null:
		return
	if _flourish_in > 0.0:
		_flourish_in -= delta
		if _flourish_in <= 0.0:
			if CASTERS.has(_class_id):
				visual.anim.trigger_cast_release(1.2)
			else:
				visual.anim.trigger_attack(1)
	visual.step(delta, 0.0, false, 0.0)
