## Spell and ability effects (Phase 9), all from res://data/effects.json (content/effects.json synced by
## tools/sync_client_content.py): casts, projectiles, impacts, heals, buffs, beams, light columns and
## novas for combat events, lasting markers for states (stunned, mezzed, rooted, fleeing, charmed), and
## the full-screen cues for our own character (ScreenCues). Effects are drawn only: the server never
## knows about them. Shapes are built from simple meshes and particles in the spell's colour, so a new
## spell needs no art: its event and school pick a look, and effects.json can override by name.
extends Node3D

const DATA_PATH: String = "res://data/effects.json"
const SCHOOLS: Array[String] = ["none", "magic", "fire", "cold", "poison", "disease"]

## Scales particle counts (the graphics quality setting): 0.25 on Low, 1 on High.
static var density: float = 1.0

static var _data: Dictionary = {}
static var _mats: Dictionary = {}
## Effects started, by effect id (tests read it).
var played: Dictionary = {}


static func data() -> Dictionary:
	if _data.is_empty():
		var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(DATA_PATH))
		_data = parsed if parsed is Dictionary else {"effects": {}}
	return _data


static func effect(id: String) -> Dictionary:
	var e: Variant = (data().get("effects", {}) as Dictionary).get(id)
	return e if e is Dictionary else {}


static func school_color(school: int) -> Color:
	var sc: Dictionary = data().get("school_colors", {}) as Dictionary
	var key: String = SCHOOLS[clampi(school, 0, SCHOOLS.size() - 1)]
	return Color.html(str(sc.get(key, "#e6e0b8")))


static func color_of(e: Dictionary, school: int) -> Color:
	var c: String = str(e.get("color", "school"))
	return school_color(school) if c == "school" or not c.is_valid_html_color() else Color.html(c)


## The effect id for a combat event of `ability` (a spell override's "impact", else the event's own).
static func impact_for(event: String, ability: String) -> String:
	var own: String = impact_override(ability)
	return own if own != "" else str((data().get("events", {}) as Dictionary).get(event, ""))


## The impact `ability` names for itself in effects.json's spells, or "".
static func impact_override(ability: String) -> String:
	var o: Variant = (data().get("spells", {}) as Dictionary).get(ability)
	if o is Dictionary and (o as Dictionary).has("impact"):
		return str((o as Dictionary)["impact"])
	return ""


## The projectile to fly for `ability`, or "" (self-cast, or a spell type that does not travel).
static func projectile_for(ability: String) -> String:
	var o: Variant = (data().get("spells", {}) as Dictionary).get(ability)
	if o is Dictionary and (o as Dictionary).has("projectile"):
		return str((o as Dictionary)["projectile"])
	var info: Variant = (data().get("by_name", {}) as Dictionary).get(ability)
	if info is Dictionary and (data().get("projectile_types", []) as Array).has(str((info as Dictionary).get("type", ""))):
		return str(data().get("projectile", ""))
	return ""


## Height of an entity's chest and of its head, from its visual's nameplate.
static func _heights(n: Node3D) -> Vector2:
	var v: Variant = n.get("visual")
	var h: float = 2.0
	if v is Node and (v as Node).has_method("nameplate_height"):
		h = float((v as Node).call("nameplate_height"))
	return Vector2(h * 0.55, h)


# ------------------------------------------------------------------------------ events

## A combat event's effect: an optional projectile or beam from `src`, then the impact on `tgt`.
## The first landing of a travelling spell flies; ticks and self-casts play in place.
func play_event(event: String, src: Node3D, tgt: Node3D, school: int, ability: String) -> void:
	if tgt == null:
		return
	var impact: String = impact_for(event, ability)
	var e: Dictionary = effect(impact)
	if e.is_empty():
		return
	var travels: bool = event in ["spell_hit", "debuff", "mez", "root", "snare", "slow", "fear", "charm"]
	if src != null and src != tgt and travels:
		var proj: String = projectile_for(ability)
		if proj != "":
			_projectile(proj, src, tgt, school, impact_override(ability))
			return
	if str(e.get("kind")) == "beam" and src != null and src != tgt:
		_beam(impact, src, tgt, school)
		return
	spawn(impact, tgt, school)


## Plays effect `id` on `at` (one-shot kinds).
func spawn(id: String, at: Node3D, school: int) -> void:
	var e: Dictionary = effect(id)
	if e.is_empty() or at == null or not is_instance_valid(at):
		return
	played[id] = int(played.get(id, 0)) + 1
	var c: Color = color_of(e, school)
	var hts: Vector2 = _heights(at)
	match str(e.get("kind")):
		"burst":
			_particles(at.global_position + Vector3(0.0, hts.x, 0.0), e, c, false)
		"rise":
			_particles(at.global_position + Vector3(0.0, 0.2, 0.0), e, c, true)
		"nova":
			_nova(at.global_position + Vector3(0.0, 0.08, 0.0), e, c)
		"column":
			_column(at.global_position, e, c, hts.y)
		"projectile", "beam":
			_particles(at.global_position + Vector3(0.0, hts.x, 0.0), e, c, false)


func _particles(pos: Vector3, e: Dictionary, c: Color, rising: bool) -> void:
	var p: CPUParticles3D = CPUParticles3D.new()
	p.one_shot = true
	p.amount = maxi(2, int(float(e.get("count", 24)) * density))
	var life: float = float(e.get("duration", 0.9))
	p.lifetime = life
	p.explosiveness = 0.85 if not rising else 0.5
	p.direction = Vector3.UP
	p.spread = 35.0 if rising else 180.0
	p.initial_velocity_min = 1.0 if rising else 2.0
	p.initial_velocity_max = 2.2 if rising else 4.0
	p.gravity = Vector3(0.0, 1.0 if rising else -3.0, 0.0)
	if rising:
		p.emission_shape = CPUParticles3D.EMISSION_SHAPE_RING
		p.emission_ring_axis = Vector3.UP
		p.emission_ring_radius = 0.6
		p.emission_ring_inner_radius = 0.3
		p.emission_ring_height = 0.1
	p.mesh = _spark(float(e.get("size", 0.07)), c)
	p.position = pos
	add_child(p)
	p.emitting = true
	get_tree().create_timer(life + 0.8).timeout.connect(p.queue_free)


func _nova(pos: Vector3, e: Dictionary, c: Color) -> void:
	var ring: MeshInstance3D = MeshInstance3D.new()
	var t: TorusMesh = TorusMesh.new()
	t.inner_radius = 0.85
	t.outer_radius = 1.0
	t.rings = 24
	t.ring_segments = 6
	ring.mesh = t
	ring.material_override = _glow(c)
	ring.position = pos
	ring.scale = Vector3(0.2, 0.15, 0.2)
	add_child(ring)
	var s: float = float(e.get("size", 2.0))
	var d: float = float(e.get("duration", 0.8))
	var tw: Tween = ring.create_tween().set_parallel(true)
	tw.tween_property(ring, "scale", Vector3(s, 0.15, s), d).set_ease(Tween.EASE_OUT)
	tw.tween_property(ring, "transparency", 1.0, d)
	tw.chain().tween_callback(ring.queue_free)


func _column(pos: Vector3, e: Dictionary, c: Color, height: float) -> void:
	var col: MeshInstance3D = MeshInstance3D.new()
	var cyl: CylinderMesh = CylinderMesh.new()
	var r: float = float(e.get("size", 1.0))
	cyl.top_radius = r * 0.7
	cyl.bottom_radius = r
	cyl.height = maxf(4.0, height * 2.5)
	cyl.cap_top = false
	cyl.cap_bottom = false
	col.mesh = cyl
	col.material_override = _glow(c)
	col.position = pos + Vector3(0.0, cyl.height * 0.5, 0.0)
	col.transparency = 1.0
	add_child(col)
	var d: float = float(e.get("duration", 2.0))
	var tw: Tween = col.create_tween()
	tw.tween_property(col, "transparency", 0.55, d * 0.25)
	tw.tween_property(col, "transparency", 1.0, d * 0.75)
	tw.tween_callback(col.queue_free)
	_particles(pos, {"count": e.get("count", 36), "size": 0.08, "duration": d}, c, true)
	_nova(pos + Vector3(0.0, 0.08, 0.0), {"size": r * 2.2, "duration": d * 0.5}, c)


## Flies projectile `id` from `src` to `tgt`, then plays `impact` there (default: the projectile's own).
func _projectile(id: String, src: Node3D, tgt: Node3D, school: int, impact: String = "") -> void:
	var e: Dictionary = effect(id)
	played[id] = int(played.get(id, 0)) + 1
	var c: Color = color_of(e, school)
	var orb: MeshInstance3D = MeshInstance3D.new()
	var sp: SphereMesh = SphereMesh.new()
	var r: float = float(e.get("size", 0.22))
	sp.radius = r
	sp.height = r * 2.0
	sp.radial_segments = 10
	sp.rings = 6
	orb.mesh = sp
	orb.material_override = _glow(c)
	var trail: CPUParticles3D = CPUParticles3D.new()
	trail.amount = maxi(4, int(24.0 * density))
	trail.lifetime = 0.35
	trail.local_coords = false
	trail.spread = 180.0
	trail.initial_velocity_min = 0.1
	trail.initial_velocity_max = 0.5
	trail.gravity = Vector3.ZERO
	trail.mesh = _spark(r * 0.45, c)
	orb.add_child(trail)
	var from: Vector3 = src.global_position + Vector3(0.0, _heights(src).x * 1.1, 0.0)
	orb.position = from
	add_child(orb)
	var to: Vector3 = tgt.global_position + Vector3(0.0, _heights(tgt).x, 0.0)
	var time: float = clampf(from.distance_to(to) / float(e.get("speed", 20.0)), 0.08, 0.6)
	var tw: Tween = orb.create_tween()
	tw.tween_property(orb, "position", to, time)
	if impact == "":
		impact = str(e.get("impact", ""))
	tw.tween_callback(func() -> void:
		if impact != "" and is_instance_valid(tgt):
			spawn(impact, tgt, school)
		orb.queue_free())


func _beam(id: String, src: Node3D, tgt: Node3D, school: int) -> void:
	var e: Dictionary = effect(id)
	played[id] = int(played.get(id, 0)) + 1
	var c: Color = color_of(e, school)
	var a: Vector3 = src.global_position + Vector3(0.0, _heights(src).x, 0.0)
	var b: Vector3 = tgt.global_position + Vector3(0.0, _heights(tgt).x, 0.0)
	var beam: MeshInstance3D = MeshInstance3D.new()
	var cyl: CylinderMesh = CylinderMesh.new()
	var w: float = float(e.get("size", 0.08))
	cyl.top_radius = w
	cyl.bottom_radius = w
	cyl.height = maxf(0.1, a.distance_to(b))
	cyl.radial_segments = 6
	beam.mesh = cyl
	beam.material_override = _glow(c)
	add_child(beam)
	beam.global_position = (a + b) * 0.5
	var dir: Vector3 = (b - a).normalized()
	if absf(dir.dot(Vector3.UP)) < 0.999:
		beam.global_basis = Basis(Quaternion(Vector3.UP, dir))
	var tw: Tween = beam.create_tween()
	tw.tween_property(beam, "transparency", 1.0, float(e.get("duration", 0.45)))
	tw.tween_callback(beam.queue_free)
	var impact: String = str(e.get("impact", ""))
	if impact != "":
		spawn(impact, tgt, school)


# ------------------------------------------------------------------------------ casting and states

## Motes around a caster while a cast lasts (the spell's override, else "cast").
func cast(at: Node3D, school: int, ability: String, on: bool) -> void:
	if at == null:
		return
	var old: Node = at.get_node_or_null("FxCast")
	if old != null:
		at.remove_child(old)  # frees its name now: a cast starting this frame must be found by the next end
		old.queue_free()
	if not on:
		return
	var id: String = "cast"
	var o: Variant = (data().get("spells", {}) as Dictionary).get(ability)
	if o is Dictionary and (o as Dictionary).has("cast"):
		id = str((o as Dictionary)["cast"])
	var e: Dictionary = effect(id)
	if e.is_empty():
		return
	played[id] = int(played.get(id, 0)) + 1
	var p: CPUParticles3D = CPUParticles3D.new()
	p.name = "FxCast"
	p.amount = maxi(4, int(float(e.get("count", 20)) * density))
	p.lifetime = 0.8
	p.emission_shape = CPUParticles3D.EMISSION_SHAPE_RING
	p.emission_ring_axis = Vector3.UP
	p.emission_ring_radius = 0.7
	p.emission_ring_inner_radius = 0.5
	p.emission_ring_height = 0.1
	p.direction = Vector3.UP
	p.spread = 10.0
	p.initial_velocity_min = 0.8
	p.initial_velocity_max = 1.4
	p.gravity = Vector3.ZERO
	p.mesh = _spark(float(e.get("size", 0.07)), color_of(e, school).lightened(0.2))
	p.position = Vector3(0.0, 0.2, 0.0)
	at.add_child(p)
	p.emitting = true


## Shows the lasting effect of every state in `on` ({state: bool}) on an entity, removing the rest.
func set_states(at: Node3D, on: Dictionary) -> void:
	if at == null:
		return
	var states: Dictionary = data().get("states", {}) as Dictionary
	for st: String in states.keys():
		var node_name: String = "FxState_" + st
		var have: Node = at.get_node_or_null(node_name)
		var want: bool = bool(on.get(st, false))
		if want and have == null:
			var n: Node3D = _state_node(str(states[st]), at)
			if n != null:
				n.name = node_name
				at.add_child(n)
		elif not want and have != null:
			at.remove_child(have)  # gone from lookups now, so the state can come back this frame
			have.queue_free()


func _state_node(id: String, at: Node3D) -> Node3D:
	var e: Dictionary = effect(id)
	if e.is_empty():
		return null
	played[id] = int(played.get(id, 0)) + 1
	var c: Color = color_of(e, 0)
	var s: float = float(e.get("size", 0.5))
	var root: Node3D = Node3D.new()
	var spin: Node3D = Node3D.new()
	root.add_child(spin)
	if str(e.get("kind")) == "aura":
		root.position = Vector3(0.0, 0.06, 0.0)
		var ring: MeshInstance3D = MeshInstance3D.new()
		var t: TorusMesh = TorusMesh.new()
		t.inner_radius = s * 0.85
		t.outer_radius = s
		t.rings = 20
		t.ring_segments = 5
		ring.mesh = t
		ring.material_override = _glow(c)
		ring.scale = Vector3(1.0, 0.2, 1.0)
		spin.add_child(ring)
		if str(e.get("shape", "")) == "vines":
			for k: int in range(6):
				var v: MeshInstance3D = MeshInstance3D.new()
				var cm: CylinderMesh = CylinderMesh.new()
				cm.top_radius = 0.0
				cm.bottom_radius = 0.05
				cm.height = 0.7
				cm.radial_segments = 5
				v.mesh = cm
				v.material_override = _glow(c.darkened(0.2))
				var a: float = TAU * float(k) / 6.0
				v.position = Vector3(cos(a) * s * 0.9, 0.3, sin(a) * s * 0.9)
				v.rotation = Vector3(0.0, -a, 0.35)
				spin.add_child(v)
	else:
		root.position = Vector3(0.0, _heights(at).y + 0.15, 0.0)
		var shape: String = str(e.get("shape", "stars"))
		var count: int = 3 if shape == "stars" else (6 if shape == "swirl" else 4)
		for k: int in range(count):
			var m: MeshInstance3D = MeshInstance3D.new()
			var sp: SphereMesh = SphereMesh.new()
			var r: float = 0.07 if shape != "swirl" else 0.05
			sp.radius = r
			sp.height = r * (2.0 if shape != "drops" else 3.0)
			sp.radial_segments = 6
			sp.rings = 4
			m.mesh = sp
			m.material_override = _glow(c)
			var a: float = TAU * float(k) / float(count)
			m.position = Vector3(cos(a) * s, sin(a * 2.0) * 0.05 if shape == "swirl" else 0.0, sin(a) * s)
			spin.add_child(m)
	var tw: Tween = spin.create_tween().set_loops()
	tw.tween_property(spin, "rotation:y", TAU, 1.4 if str(e.get("kind")) == "marker" else 4.0).from(0.0)
	return root


# ------------------------------------------------------------------------------ materials

static func _glow(c: Color) -> StandardMaterial3D:
	var key: String = "g:" + c.to_html()
	if not _mats.has(key):
		var m: StandardMaterial3D = StandardMaterial3D.new()
		m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		# Plain alpha blending keeps a school's colour (adding light washed bright scenes to white).
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		m.albedo_color = Color(c.r, c.g, c.b, 0.85)
		m.cull_mode = BaseMaterial3D.CULL_DISABLED
		_mats[key] = m
	return _mats[key] as StandardMaterial3D


static func _spark(size: float, c: Color) -> Mesh:
	var m: SphereMesh = SphereMesh.new()
	m.radius = size
	m.height = size * 2.0
	m.radial_segments = 6
	m.rings = 3
	m.material = _glow(c)
	return m
