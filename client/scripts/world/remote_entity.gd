## Another player, a mob, a corpse, or an NPC (trainer). Moving entities render slightly in the past by
## interpolating buffered server snapshots. The body is a CharacterVisual (a content-driven model or
## its placeholder) animated from server state: speed from the interpolated motion, the dead/sitting/
## stunned flags, combat, casts, and combat events (attacks and hits).
extends Node3D

const CharacterVisual := preload("res://scripts/world/character_visual.gd")
const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const MAX_BUFFER: int = 40
## How long an entity keeps facing the target of its last swing or spell.
const FACE_SECONDS: float = 4.0

const KIND_PLAYER: int = 0
const KIND_MOB: int = 1
const KIND_CORPSE: int = 2
const KIND_NPC: int = 3

const FLAG_DEAD: int = 1
const FLAG_IN_COMBAT: int = 2
const FLAG_SITTING: int = 4
const FLAG_STUNNED: int = 8
const FLAG_FLEEING: int = 16
const FLAG_MEZZED: int = 64
# flags2 (Phase 4)
const FLAG2_FEIGNED: int = 1
const FLAG2_HIDDEN: int = 2
const FLAG2_SNEAKING: int = 4
const FLAG2_SINGING: int = 8
const FLAG2_PET: int = 16
const FLAG2_CHARMED: int = 32

var entity_id: int = 0
var kind: int = KIND_PLAYER
var display_name: String = ""
## Players: the race id. Mobs, NPCs and mob corpses: the model id.
var model: String = ""
var class_id: String = ""
var gender: int = 0
var level: int = 1
var hp_pct: int = 100
var flags: int = 0
var flags2: int = 0
## Players under an illusion of a creature: the model to draw instead (a race illusion arrives as `model`).
var illusion_model: String = ""
## Pets: the owning player's entity id (0 = wild).
var owner_id: int = 0
var scale_factor: float = 1.0
## A corpse spawned right where something just died plays its death clip instead of appearing dead.
var fresh_death: bool = false
var visual: CharacterVisual
var _buffer: Array[Dictionary] = []
var _tag: Label3D
var _last_pos: Vector3 = Vector3.ZERO
var _has_last: bool = false
var _speed: float = 0.0
var _backward: bool = false
var _face_target: WeakRef = null
var _face_until: float = 0.0
var _casting_at: WeakRef = null
var _now: float = 0.0


func setup(spawn: Dictionary, server_time: float) -> void:
	entity_id = int(spawn["entity_id"])
	_apply_spawn(spawn)
	var pos: Vector3 = Vector3(float(spawn["x"]), float(spawn["y"]), float(spawn["z"]))
	position = pos
	rotation.y = float(spawn["yaw"])
	push_state(server_time, pos, float(spawn["yaw"]), hp_pct, flags, flags2)


func _apply_spawn(spawn: Dictionary) -> void:
	kind = int(spawn["kind"])
	display_name = str(spawn["name"])
	model = str(spawn["race"])
	class_id = str(spawn["class_id"])
	gender = int(spawn.get("gender", 0))
	level = int(spawn["level"])
	hp_pct = int(spawn["hp_pct"])
	flags = int(spawn["flags"])
	flags2 = int(spawn.get("flags2", 0))
	illusion_model = str(spawn.get("model", ""))
	owner_id = int(spawn.get("owner_id", 0))
	scale_factor = clampf(float(spawn["scale"]), 0.1, 10.0)


## A repeated EntitySpawn for a known id refreshes name/level/state without a respawn; a new look
## (an illusion starting or ending, a charmed mob changing hands) rebuilds the body.
func refresh(spawn: Dictionary) -> void:
	var before: Dictionary = look()
	_apply_spawn(spawn)
	if visual != null and look() != before:
		_rebuild_visual()
	if _tag != null:
		_tag.text = _tag_text()
		_tag.modulate = _tag_color()
	_sync_flags(false)


func _rebuild_visual() -> void:
	if visual != null:
		remove_child(visual)
		visual.queue_free()
	visual = CharacterVisual.new()
	visual.name = "Visual"
	visual.build(look(), scale_factor)
	add_child(visual)
	move_child(visual, 0)
	if _tag != null:
		_tag.position = Vector3(0.0, visual.nameplate_height(), 0.0)
	_sync_flags(true)


## What this entity looks like: players (and fresh player corpses, which carry the owner's class)
## by race, gender and class; everything else by its model id.
func look() -> Dictionary:
	var is_player_look: bool = kind == KIND_PLAYER or (kind == KIND_CORPSE and class_id != "")
	if kind == KIND_PLAYER and illusion_model != "":
		return VisualLibrary.mob_look(illusion_model)
	if is_player_look:
		return VisualLibrary.player_look(model, gender, class_id)
	return VisualLibrary.mob_look(model)


func _ready() -> void:
	visual = CharacterVisual.new()
	visual.name = "Visual"
	visual.build(look(), scale_factor)
	add_child(visual)
	_tag = Label3D.new()
	_tag.text = _tag_text()
	_tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	var tag_y: float = visual.nameplate_height()
	if kind == KIND_CORPSE:
		tag_y = maxf(0.6, tag_y * 0.4)
	_tag.position = Vector3(0.0, tag_y, 0.0)
	_tag.font_size = 36 if kind != KIND_CORPSE else 28
	_tag.outline_size = 10
	_tag.modulate = _tag_color()
	_tag.no_depth_test = true
	add_child(_tag)
	if kind == KIND_CORPSE:
		visual.anim.set_dead(true, not fresh_death)
	_sync_flags(true)


func _tag_text() -> String:
	if kind == KIND_NPC and class_id != "":
		return "%s\n<%s>" % [display_name, class_id.capitalize()]
	if kind == KIND_MOB and (flags2 & FLAG2_CHARMED) != 0:
		return "%s\n<charmed>" % display_name
	return display_name


func _tag_color() -> Color:
	match kind:
		KIND_MOB:
			if (flags2 & FLAG2_PET) != 0:
				return Color(0.6, 1.0, 0.85)
			return Color(1.0, 0.75, 0.6)
		KIND_CORPSE:
			return Color(0.7, 0.7, 0.7)
		KIND_NPC:
			return Color(0.6, 1.0, 0.7)
	return Color(0.85, 0.92, 1.0)


## Push the server flags into the animation state. `initial`: we just arrived, so someone already
## dead shows the finished death pose instead of dying again in front of us.
func _sync_flags(initial: bool) -> void:
	if visual == null:
		return
	var a: RefCounted = visual.anim
	if kind != KIND_CORPSE:
		# Feigning death looks like death (that is the point).
		visual.anim.set_dead((flags & FLAG_DEAD) != 0 or (flags2 & FLAG2_FEIGNED) != 0, initial)
	set_ghost(visual, (flags2 & FLAG2_HIDDEN) != 0)
	a.set("sitting", (flags & FLAG_SITTING) != 0)
	a.set("stunned", (flags & (FLAG_STUNNED | FLAG_MEZZED)) != 0)
	a.set("in_combat", (flags & FLAG_IN_COMBAT) != 0)


func is_alive() -> bool:
	return kind != KIND_CORPSE and (flags & FLAG_DEAD) == 0


func is_pet() -> bool:
	return kind == KIND_MOB and (flags2 & FLAG2_PET) != 0


## Hidden and invisible characters are drawn faintly (to the people who can still see them at all).
static func set_ghost(root: Node, on: bool) -> void:
	if root == null:
		return
	for n: Node in root.find_children("*", "GeometryInstance3D", true, false):
		(n as GeometryInstance3D).transparency = 0.7 if on else 0.0


## Body radius in metres (click picking, selection).
func radius() -> float:
	return visual.collision_radius() if visual != null else 0.5 * scale_factor


## The point to aim at when picking or projecting: mid-body.
func pick_point() -> Vector3:
	var h: float = visual.nameplate_height() if visual != null else 1.8 * scale_factor
	return global_position + Vector3(0.0, h * (0.2 if kind == KIND_CORPSE else 0.45), 0.0)


func set_selected(selected: bool) -> void:
	if _tag != null:
		_tag.modulate = Color(1.0, 1.0, 0.55) if selected else _tag_color()
		_tag.outline_modulate = Color(0.35, 0.2, 0.0) if selected else Color.BLACK


func push_state(t: float, pos: Vector3, yaw: float, hp: int, f: int, f2: int = 0) -> void:
	hp_pct = hp
	if f != flags or f2 != flags2:
		var tag_changed: bool = (f2 & (FLAG2_PET | FLAG2_CHARMED)) != (flags2 & (FLAG2_PET | FLAG2_CHARMED))
		flags = f
		flags2 = f2
		_sync_flags(false)
		if tag_changed and _tag != null:
			_tag.text = _tag_text()
			_tag.modulate = _tag_color()
	if _buffer.size() > 0 and t <= float(_buffer.back()["t"]):
		return
	_buffer.append({"t": t, "pos": pos, "yaw": yaw})
	if _buffer.size() > MAX_BUFFER:
		_buffer.pop_front()


# ------------------------------------------------------------------------------ animation events

## A melee swing (or ability) by this entity at `target`.
func on_attack(target: Node3D, ability: bool) -> void:
	_face(target)
	if visual != null:
		visual.anim.trigger_attack(2 if ability else 0)


func on_hit() -> void:
	if visual != null:
		visual.anim.trigger_hit()


func on_cast_start(target: Node3D) -> void:
	if visual != null:
		visual.anim.casting = true
	_casting_at = weakref(target) if target != null and target != self else null


func on_cast_end(success: bool, instant: bool) -> void:
	if visual != null:
		visual.anim.casting = false
		if success and instant:
			visual.anim.trigger_cast_release()
	var t: Node3D = _casting_at.get_ref() as Node3D if _casting_at != null else null
	if success and t != null:
		_face(t)
	_casting_at = null


func _face(target: Node3D) -> void:
	if target == null or target == self:
		return
	_face_target = weakref(target)
	_face_until = _now + FACE_SECONDS


## Yaw toward whatever we are fighting or casting at, or NAN.
func face_yaw() -> float:
	var t: Node3D = null
	if _casting_at != null:
		t = _casting_at.get_ref() as Node3D
	if t == null and _face_target != null and _now < _face_until:
		t = _face_target.get_ref() as Node3D
	if t == null or not is_instance_valid(t):
		return NAN
	var d: Vector3 = t.global_position - global_position
	if Vector2(d.x, d.z).length() < 0.05:
		return NAN
	return atan2(-d.x, -d.z)


# ------------------------------------------------------------------------------ per frame

func update_render(render_time: float) -> void:
	if _buffer.is_empty() or kind == KIND_CORPSE or kind == KIND_NPC:
		return
	var first: Dictionary = _buffer[0]
	if render_time <= float(first["t"]):
		_apply(first["pos"], float(first["yaw"]))
		return
	for i: int in range(_buffer.size() - 1):
		var a: Dictionary = _buffer[i]
		var b: Dictionary = _buffer[i + 1]
		var tb: float = float(b["t"])
		if render_time <= tb:
			var ta: float = float(a["t"])
			var w: float = clampf((render_time - ta) / maxf(tb - ta, 0.0001), 0.0, 1.0)
			var pa: Vector3 = a["pos"]
			var pb: Vector3 = b["pos"]
			_apply(pa.lerp(pb, w), lerp_angle(float(a["yaw"]), float(b["yaw"]), w))
			# Drop samples we will never need again.
			for _n: int in range(i):
				_buffer.pop_front()
			return
	var last: Dictionary = _buffer.back()
	_apply(last["pos"], float(last["yaw"]))


## Animate: ground speed from how far we moved this frame (smoothed), then the visual's state machine.
func update_visual(delta: float) -> void:
	_now += delta
	if visual == null or delta <= 0.0:
		return
	if _has_last:
		var v: Vector3 = (position - _last_pos) / delta
		v.y = 0.0
		var k: float = 1.0 - exp(-delta * 10.0)
		var inst: float = v.length()
		if inst > 40.0:
			inst = 0.0  # a teleport, not a sprint
		_speed = lerpf(_speed, inst, k)
		var fwd: Vector3 = Vector3(-sin(rotation.y), 0.0, -cos(rotation.y))
		_backward = inst > 0.3 and v.normalized().dot(fwd) < -0.3
	_last_pos = position
	_has_last = true
	visual.step(delta, _speed, _backward, rotation.y, face_yaw())


func _apply(pos: Vector3, yaw: float) -> void:
	position = pos
	rotation.y = yaw
