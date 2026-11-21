## The visible body of a character or creature: a glTF/GLB model from content (or the model's
## placeholder shape when the file is missing), with material tints, swappable parts, items on
## skeleton attachment points, an animation state machine driven by server state, playback speed
## matched to ground speed, and smooth turning. Everything comes from res://data/visuals.json, so
## swapping a model is a content edit (see docs/MODELS.md).
extends Node3D

const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const AnimState := preload("res://scripts/world/anim_state.gd")

const TURN_RATE: float = 12.0  # exponential turn smoothing (1/s)
const FACE_MAX_SPEED: float = 0.5  # m/s: above this, face the way we move, not the target
## Where held items sit on a placeholder body (no skeleton), in the placeholder's own frame (-Z forward).
const PLACEHOLDER_POINTS: Dictionary = {
	"main_hand": Vector3(-0.48, 0.95, -0.12), "off_hand": Vector3(0.5, 1.05, -0.05),
	"off_hand_held": Vector3(0.48, 0.95, -0.12), "back": Vector3(0.0, 1.3, 0.42), "head": Vector3(0.0, 1.95, 0.0),
}
const LOW_PLACEHOLDER_POINTS: Dictionary = {"head": Vector3(0.0, 0.95, -0.7), "back": Vector3(0.0, 0.95, 0.0)}

## Process-wide counters for the autotest report and the client tests.
static var stats: Dictionary = {"models": 0, "placeholders": 0, "attachments": 0, "missing_clips": 0}
static var states_seen: Dictionary = {}
static var _tint_cache: Dictionary = {}

var look: Dictionary = {}
var model_def: Dictionary = {}
var anim: AnimState = AnimState.new()
var entity_scale: float = 1.0
var is_placeholder: bool = true
var placeholder_shape: String = "upright"
## Attachment holders by slot (main_hand, off_hand, back, head).
var attached: Dictionary = {}
## The resolved state now playing (after fallbacks), for tests and the autotest.
var playing: String = ""

var _body: Node3D
var _instance: Node3D
var _player: AnimationPlayer
var _skeleton: Skeleton3D
var _clips: Dictionary = {}  # resolved standard state -> {clip, ground_speed, loop}
var _blend: float = 0.15
var _played_serial: int = -1
var _vis_yaw: float = 0.0
var _yaw_ready: bool = false
var _size: float = 1.0
var _base_scale: Vector3 = Vector3.ONE
var _base_yaw: float = 0.0
var _phase: float = 0.0
var _oneshot_t: float = 0.0
var _proc_serial: int = -1
var _death_t: float = 0.0
var _sit_t: float = 0.0
var _time: float = 0.0


## Build from a look (VisualLibrary.player_look / mob_look) at an entity scale.
func build(a_look: Dictionary, a_entity_scale: float = 1.0) -> void:
	look = a_look
	model_def = look.get("model", {}) as Dictionary
	entity_scale = clampf(a_entity_scale, 0.05, 20.0)
	var look_scale: Vector3 = look.get("scale", Vector3.ONE)
	_body = Node3D.new()
	_body.name = "Body"
	add_child(_body)
	_base_scale = look_scale * float(model_def.get("scale", 1.0)) * entity_scale
	_size = _base_scale.y
	_body.scale = _base_scale
	var ph: Dictionary = model_def.get("placeholder", {}) as Dictionary
	placeholder_shape = str(ph.get("shape", "upright"))
	var ps: PackedScene = VisualLibrary.load_scene(str(model_def.get("scene", "")))
	if ps != null:
		_instance = ps.instantiate() as Node3D
	if _instance != null:
		is_placeholder = false
		_base_yaw = deg_to_rad(float(model_def.get("yaw_offset_deg", 0.0)))
		_body.add_child(_instance)
		_setup_model()
		stats["models"] = int(stats["models"]) + 1
	else:
		is_placeholder = true
		_base_yaw = 0.0
		_build_placeholder(ph)
		stats["placeholders"] = int(stats["placeholders"]) + 1
	_body.rotation.y = _base_yaw
	var held: Dictionary = look.get("held", {}) as Dictionary
	for slot: String in held.keys():
		attach(slot, held[slot] as Dictionary)


func nameplate_height() -> float:
	var look_scale: Vector3 = look.get("scale", Vector3.ONE)
	return float(model_def.get("nameplate_height", 2.2)) * look_scale.y * entity_scale


func collision_radius() -> float:
	var look_scale: Vector3 = look.get("scale", Vector3.ONE)
	return float(model_def.get("collision_radius", 0.5)) * maxf(look_scale.x, look_scale.z) * entity_scale


func has_clip_for(state: String) -> bool:
	return _clips.has(state)


func skeleton() -> Skeleton3D:
	return _skeleton


func model_instance() -> Node3D:
	return _instance


# ------------------------------------------------------------------------------ model setup

func _setup_model() -> void:
	var players: Array[Node] = _instance.find_children("*", "AnimationPlayer", true, false)
	_player = players[0] as AnimationPlayer if players.size() > 0 else null
	var skels: Array[Node] = _instance.find_children("*", "Skeleton3D", true, false)
	_skeleton = skels[0] as Skeleton3D if skels.size() > 0 else null
	_apply_parts()
	_apply_tints()
	var map: Dictionary = VisualLibrary.anim_map(str(model_def.get("animations", "")))
	_blend = clampf(float(map.get("blend", 0.15)), 0.0, 2.0)
	anim.walk_run_threshold = float(map.get("walk_run_threshold", 3.2))
	var clips: Dictionary = map.get("clips", {}) as Dictionary
	for state: String in clips.keys():
		var spec: Dictionary = clips[state] as Dictionary
		var clip: String = runtime_clip_name(_player, str(spec.get("clip", "")))
		if _player == null or not _player.has_animation(clip):
			stats["missing_clips"] = int(stats["missing_clips"]) + 1
			continue
		var loop: bool = bool(spec.get("loop", AnimState.LOOPING.has(state)))
		var a: Animation = _player.get_animation(clip)
		a.loop_mode = Animation.LOOP_LINEAR if loop else Animation.LOOP_NONE
		_clips[state] = {"clip": clip, "ground_speed": float(spec.get("ground_speed", 0.0)), "loop": loop}
		anim.available[state] = true


## Godot's glTF importer drops a "_Loop"/"-Loop" (or "_Cycle") suffix from clip names and marks those
## clips looping, so a map may name a clip either way (as in the file, or as Godot shows it).
static func runtime_clip_name(player: AnimationPlayer, clip: String) -> String:
	if player == null or player.has_animation(clip):
		return clip
	var lower: String = clip.to_lower()
	for suffix: String in ["_loop", "-loop", "_cycle", "-cycle"]:
		if lower.ends_with(suffix) and player.has_animation(clip.left(clip.length() - suffix.length())):
			return clip.left(clip.length() - suffix.length())
	return clip


func _apply_parts() -> void:
	var groups: Dictionary = model_def.get("parts", {}) as Dictionary
	var chosen: Dictionary = look.get("parts", {}) as Dictionary
	for group: String in groups.keys():
		for part: Variant in groups[group]:
			var n: Node = _instance.find_child(str(part), true, false)
			if n is Node3D:
				(n as Node3D).visible = str(chosen.get(group, "")) == str(part)


func _apply_tints() -> void:
	var tints: Dictionary = look.get("tints", {}) as Dictionary
	if tints.is_empty():
		return
	for node: Node in _instance.find_children("*", "MeshInstance3D", true, false):
		var mi: MeshInstance3D = node as MeshInstance3D
		if mi.mesh == null:
			continue
		for i: int in range(mi.mesh.get_surface_count()):
			var mat: Material = mi.mesh.surface_get_material(i)
			if mat == null:
				continue
			var c: Variant = tints.get(mat.resource_name, tints.get("*"))
			if c is Color:
				mi.set_surface_override_material(i, _tinted(mat, c))


static func _tinted(mat: Material, c: Color) -> Material:
	var key: String = "%d:%s" % [mat.get_instance_id(), c.to_html()]
	if _tint_cache.has(key):
		return _tint_cache[key] as Material
	var dup: Material = mat.duplicate() as Material
	var base: BaseMaterial3D = dup as BaseMaterial3D
	if base != null:
		base.albedo_color = Color(c.r, c.g, c.b, base.albedo_color.a)
	_tint_cache[key] = dup
	return dup


# ------------------------------------------------------------------------------ attachments

## Attach an item visual ({scene, slot, attach_point?, position?, rotation_deg?, scale?}) to its slot.
## Returns false (leaving the slot empty) when the mesh or the attachment point is missing.
func attach(slot: String, visual: Dictionary) -> bool:
	detach(slot)
	var point: String = str(visual.get("attach_point", slot))
	var ps: PackedScene = VisualLibrary.load_scene(str(visual.get("scene", "")))
	if ps == null:
		return false
	var item: Node3D = ps.instantiate() as Node3D
	if item == null:
		return false
	item.position = _vec3(visual.get("position"), Vector3.ZERO)
	item.rotation_degrees = _vec3(visual.get("rotation_deg"), Vector3.ZERO)
	var s: Variant = visual.get("scale", 1.0)
	item.scale = _vec3(s, Vector3.ONE) if s is Array else Vector3.ONE * clampf(float(s), 0.05, 20.0)
	var holder: Node3D = Node3D.new()
	holder.name = "Held_" + slot
	holder.add_child(item)
	if not is_placeholder:
		var points: Dictionary = model_def.get("attachments", {}) as Dictionary
		var spec: Variant = points.get(point)
		if spec is String:
			spec = {"bone": spec}
		if not spec is Dictionary or _skeleton == null or _skeleton.find_bone(str(spec["bone"])) < 0:
			push_warning("visuals: model '%s' has no attachment point '%s'" % [str(model_def.get("id", "")), point])
			holder.free()
			return false
		var ba: BoneAttachment3D = BoneAttachment3D.new()
		ba.name = "Attach_" + slot
		ba.bone_name = str(spec["bone"])
		_skeleton.add_child(ba)
		holder.position = _vec3(spec.get("position"), Vector3.ZERO)
		holder.rotation_degrees = _vec3(spec.get("rotation_deg"), Vector3.ZERO)
		ba.add_child(holder)
		attached[slot] = ba
	else:
		var table: Dictionary = LOW_PLACEHOLDER_POINTS if placeholder_shape == "low" else PLACEHOLDER_POINTS
		if not table.has(point):
			holder.free()
			return false
		holder.position = table[point]
		# Placeholders face -Z; items are authored for +Z-facing models.
		holder.rotation.y = PI
		_body.add_child(holder)
		attached[slot] = holder
	stats["attachments"] = int(stats["attachments"]) + 1
	return true


func detach(slot: String) -> void:
	if attached.has(slot):
		var n: Node = attached[slot]
		attached.erase(slot)
		if is_instance_valid(n):
			n.queue_free()


static func _vec3(v: Variant, fallback: Vector3) -> Vector3:
	if v is Array and (v as Array).size() == 3:
		return Vector3(float(v[0]), float(v[1]), float(v[2]))
	return fallback


# ------------------------------------------------------------------------------ per frame

## speed: ground speed in m/s; backward: moving against our facing; root_yaw: the entity's yaw;
## face_yaw: the yaw to face while standing (NAN = the root yaw).
func step(delta: float, speed: float, backward: bool, root_yaw: float, face_yaw: float = NAN) -> void:
	_time += delta
	anim.speed = speed
	anim.backward = backward
	var state: String = anim.update(delta)
	states_seen[state] = true
	if is_placeholder:
		playing = state
	else:
		_play(state)
	_procedural(state, delta)
	var desired: float = root_yaw
	if not is_nan(face_yaw) and speed < FACE_MAX_SPEED and not anim.dead and not anim.sitting:
		desired = face_yaw
	if not _yaw_ready:
		_vis_yaw = desired
		_yaw_ready = true
	_vis_yaw = lerp_angle(_vis_yaw, desired, 1.0 - exp(-TURN_RATE * delta))
	rotation.y = wrapf(_vis_yaw - root_yaw, -PI, PI)


func _play(state: String) -> void:
	var resolved: String = anim.resolve(state)
	if resolved == "" and state != "death" and state != "sit":
		# No clip for this one-shot (say, a hit reaction): keep standing or moving instead.
		resolved = anim.resolve("combat_idle" if anim.in_combat else "idle")
	if resolved == "":
		if _player != null and playing != state and (state == "death" or state == "sit"):
			_player.pause()  # no clip at all: the procedural layer poses the body
		playing = state
		return
	var spec: Dictionary = _clips[resolved]
	var restart: bool = resolved != playing
	if anim.oneshot() != "" and state == anim.oneshot() and anim.oneshot_serial != _played_serial:
		restart = true  # a new swing restarts even the same clip
	if restart:
		var clip: String = str(spec["clip"])
		var same_clip: bool = _player.current_animation == clip
		_player.play(clip, _blend)
		if anim.oneshot() != "" and state == anim.oneshot():
			if same_clip:
				_player.seek(0.0, true)  # play() does not rewind a clip that is already playing
			anim.set_oneshot_length(_player.current_animation_length)
			_played_serial = anim.oneshot_serial
		if state == "death" and anim.death_is_instant():
			_player.seek(_player.current_animation_length, true)
		playing = resolved
	_player.speed_scale = anim.speed_scale(resolved, float(spec["ground_speed"]), _size)


## Motion that needs no clip: placeholders bob, lunge, flinch, lie down and squat; real models only
## use it for a missing death or sit clip.
func _procedural(state: String, delta: float) -> void:
	var dead_target: float = 1.0 if state == "death" else 0.0
	var sit_target: float = 1.0 if state == "sit" else 0.0
	var proc_death: bool = is_placeholder or not _clips.has("death")
	var proc_sit: bool = is_placeholder or not _clips.has("sit")
	if anim.death_is_instant() and state == "death":
		_death_t = 1.0
	_death_t = move_toward(_death_t, dead_target, delta * 2.5) if proc_death else 0.0
	_sit_t = move_toward(_sit_t, sit_target, delta * 4.0) if proc_sit else 0.0
	var pos: Vector3 = Vector3.ZERO
	var rot: Vector3 = Vector3(0.0, _base_yaw, 0.0)
	var scl: Vector3 = _base_scale
	if is_placeholder:
		var moving: bool = state in ["walk", "walk_back", "run"]
		if moving:
			_phase += delta * maxf(anim.speed, 1.0) * 2.2 / maxf(0.3, _size)
			pos.y += absf(sin(_phase)) * 0.07 * _size
		# Each new one-shot (even the same gesture again) restarts the procedural motion.
		if anim.oneshot() == "" or anim.oneshot_serial != _proc_serial:
			_oneshot_t = 0.0
			_proc_serial = anim.oneshot_serial
		if state in ["attack1", "attack2"]:
			_oneshot_t += delta
			pos.z -= sin(clampf(_oneshot_t / 0.45, 0.0, 1.0) * PI) * 0.3 * _size
			rot.x -= sin(clampf(_oneshot_t / 0.45, 0.0, 1.0) * PI) * 0.25
		elif state == "hit":
			_oneshot_t += delta
			rot.x += sin(clampf(_oneshot_t / 0.35, 0.0, 1.0) * PI) * 0.3
		elif state == "cast":
			rot.z += sin(_time * 7.0) * 0.08
			pos.y += 0.05 * _size
	if _death_t > 0.0:
		var k: float = smoothstep(0.0, 1.0, _death_t)
		if placeholder_shape == "low":
			rot.z += k * PI / 2.0
			pos.y += k * 0.35 * _size
		else:
			rot.x += -k * PI / 2.0
			pos.y += k * 0.4 * _size
	if _sit_t > 0.0:
		var k2: float = smoothstep(0.0, 1.0, _sit_t)
		scl.y *= lerpf(1.0, 0.55, k2)
	_body.position = pos
	_body.rotation = rot
	_body.scale = scl


# ------------------------------------------------------------------------------ placeholder

func _build_placeholder(ph: Dictionary) -> void:
	var tints: Dictionary = look.get("tints", {}) as Dictionary
	var color: Color = Color.html(str(ph.get("color", "#998080"))) if str(ph.get("color", "")).is_valid_html_color() else Color(0.6, 0.5, 0.5)
	if tints.get("Cloth") is Color:
		color = tints["Cloth"]  # players: the class colour reads best on a capsule
	var body: MeshInstance3D = MeshInstance3D.new()
	body.name = "PlaceholderBody"
	var capsule: CapsuleMesh = CapsuleMesh.new()
	capsule.radius = 0.4
	capsule.height = 1.8
	body.mesh = capsule
	body.material_override = _mat(color)
	_body.add_child(body)
	if placeholder_shape == "low":
		capsule.radius = 0.45
		body.rotation.x = PI / 2.0
		body.position = Vector3(0.0, 0.5, 0.0)
		var eye_mat: StandardMaterial3D = _mat(Color(1.0, 0.9, 0.3))
		eye_mat.emission_enabled = true
		eye_mat.emission = Color(1.0, 0.6, 0.1)
		for side: float in [-1.0, 1.0]:
			var eye: MeshInstance3D = MeshInstance3D.new()
			var s: SphereMesh = SphereMesh.new()
			s.radius = 0.07
			s.height = 0.14
			eye.mesh = s
			eye.material_override = eye_mat
			eye.position = Vector3(side * 0.15, 0.7, -0.85)
			_body.add_child(eye)
	else:
		body.position = Vector3(0.0, 0.9, 0.0)
		var nose: MeshInstance3D = MeshInstance3D.new()
		var box: BoxMesh = BoxMesh.new()
		box.size = Vector3(0.22, 0.18, 0.4)
		nose.mesh = box
		nose.position = Vector3(0.0, 1.45, -0.38)
		nose.material_override = _mat(color.darkened(0.6))
		_body.add_child(nose)


static func _mat(c: Color) -> StandardMaterial3D:
	var m: StandardMaterial3D = StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = 0.8
	return m
