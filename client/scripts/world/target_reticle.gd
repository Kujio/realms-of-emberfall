## The target reticle (Phase 16): a ring on the ground under whatever you have targeted, with four marks that
## turn slowly around it, so the target is plain to see in a crowd. Its colour tells what the target is:
## a monster's "con" colour once the server's TargetInfo arrives, green for players, gold for NPCs, grey for
## corpses. One reticle exists; the world moves it to the current target every frame.
extends Node3D

const SPIN_SPEED: float = 0.9      # radians per second
const PULSE_SPEED: float = 3.0     # the ring breathes a little so it reads against any ground
const MIN_RADIUS: float = 0.55
const HEIGHT: float = 0.06         # above the target's feet
const PLAYER_COLOR: Color = Color(0.45, 0.95, 0.55)
const NPC_COLOR: Color = Color(1.0, 0.82, 0.35)
const CORPSE_COLOR: Color = Color(0.6, 0.6, 0.6)
const MOB_COLOR: Color = Color(1.0, 1.0, 1.0)  # until the con is known

var color: Color = MOB_COLOR
var radius: float = 1.0
var _ring: MeshInstance3D
var _marks: Node3D
var _mat: StandardMaterial3D
var _t: float = 0.0


## The ring's radius for an entity of collision radius `r`: a little wider than the body, never tiny.
static func ring_radius(r: float) -> float:
	return maxf(MIN_RADIUS, r * 1.35 + 0.15)


## The reticle's colour for an entity kind (RemoteEntity.KIND_*) and, for a monster, its con (0..5, -1 unknown).
static func color_for(kind: int, con: int, con_colors: Array[Color]) -> Color:
	match kind:
		0:
			return PLAYER_COLOR
		2:
			return CORPSE_COLOR
		3:
			return NPC_COLOR
		_:
			return con_colors[clampi(con, 0, con_colors.size() - 1)] if con >= 0 and not con_colors.is_empty() else MOB_COLOR


func _ready() -> void:
	_mat = StandardMaterial3D.new()
	_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	_mat.albedo_color = color
	_ring = MeshInstance3D.new()
	var torus: TorusMesh = TorusMesh.new()
	torus.inner_radius = 0.9
	torus.outer_radius = 1.0
	torus.rings = 48
	torus.ring_segments = 6
	_ring.mesh = torus
	_ring.material_override = _mat
	_ring.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_ring.scale = Vector3(1.0, 0.15, 1.0)  # flat on the ground
	add_child(_ring)
	_marks = Node3D.new()
	add_child(_marks)
	for i: int in 4:
		var m: MeshInstance3D = MeshInstance3D.new()
		var prism: PrismMesh = PrismMesh.new()
		prism.size = Vector3(0.22, 0.22, 0.04)
		m.mesh = prism
		m.material_override = _mat
		m.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var a: float = TAU * float(i) / 4.0
		# Lying flat, pointing in at the target.
		m.position = Vector3(sin(a) * 1.18, 0.0, cos(a) * 1.18)
		m.rotation = Vector3(-PI / 2.0, a + PI, 0.0)
		_marks.add_child(m)
	visible = false


func set_color(c: Color) -> void:
	color = c
	if _mat != null:
		_mat.albedo_color = Color(c.r, c.g, c.b, _mat.albedo_color.a)


## Moves the reticle under `target` (a RemoteEntity); null hides it.
func follow(target: Node3D, delta: float) -> void:
	if target == null or not is_instance_valid(target):
		visible = false
		return
	visible = true
	_t += delta
	var r: float = MIN_RADIUS
	if target.has_method("radius"):
		r = ring_radius(float(target.call("radius")))
	radius = r
	global_position = target.global_position + Vector3(0.0, HEIGHT, 0.0)
	_ring.scale = Vector3(r, 0.15, r)
	_marks.scale = Vector3(r, 1.0, r)
	_marks.rotation.y = wrapf(_marks.rotation.y + delta * SPIN_SPEED, -PI, PI)
	if _mat != null:
		_mat.albedo_color = Color(color.r, color.g, color.b, 0.7 + 0.25 * sin(_t * PULSE_SPEED))
