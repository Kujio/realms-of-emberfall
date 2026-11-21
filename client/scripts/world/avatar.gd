## Placeholder visuals: capsule characters with a facing marker, simple mob shapes by model
## family, and corpses lying flat. Real art arrives later; these only need to read clearly.
extends RefCounted

## Model families whose bodies are horizontal (four legs, coils, or carapaces).
const LOW_MODELS: Array[String] = ["rat", "beetle", "wolf", "snake", "boar", "spider", "drake"]

const MODEL_COLORS: Dictionary = {
	"rat": Color(0.55, 0.36, 0.24), "beetle": Color(0.3, 0.2, 0.4), "wolf": Color(0.5, 0.5, 0.52),
	"snake": Color(0.35, 0.55, 0.25), "humanoid": Color(0.75, 0.6, 0.45), "skeleton": Color(0.88, 0.85, 0.75),
	"boar": Color(0.45, 0.3, 0.2), "spider": Color(0.15, 0.13, 0.15), "golem": Color(0.35, 0.5, 0.3),
	"imp": Color(0.95, 0.45, 0.15), "giant": Color(0.6, 0.5, 0.4), "bird": Color(0.3, 0.4, 0.65),
	"horror": Color(0.2, 0.35, 0.25), "treant": Color(0.4, 0.3, 0.18), "drake": Color(0.45, 0.15, 0.12),
	"wraith": Color(0.7, 0.85, 0.95),
}


static func color_for_class(class_id: String) -> Color:
	var h: int = absi(class_id.hash())
	return Color.from_hsv(float(h % 360) / 360.0, 0.55, 0.85)


static func _mat(c: Color) -> StandardMaterial3D:
	var m: StandardMaterial3D = StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = 0.8
	return m


static func build(parent: Node3D, body_color: Color, label_text: String, label_color: Color) -> void:
	var body: MeshInstance3D = MeshInstance3D.new()
	var capsule: CapsuleMesh = CapsuleMesh.new()
	capsule.radius = 0.4
	capsule.height = 1.8
	body.mesh = capsule
	body.position = Vector3(0.0, 0.9, 0.0)
	body.material_override = _mat(body_color)
	parent.add_child(body)

	var nose: MeshInstance3D = MeshInstance3D.new()
	var box: BoxMesh = BoxMesh.new()
	box.size = Vector3(0.22, 0.18, 0.4)
	nose.mesh = box
	nose.position = Vector3(0.0, 1.45, -0.38)
	nose.material_override = _mat(body_color.darkened(0.6))
	parent.add_child(nose)

	if label_text != "":
		var tag: Label3D = Label3D.new()
		tag.text = label_text
		tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		tag.position = Vector3(0.0, 2.25, 0.0)
		tag.font_size = 40
		tag.outline_size = 10
		tag.modulate = label_color
		tag.no_depth_test = true
		parent.add_child(tag)


static func build_mob(parent: Node3D, model: String, scale: float) -> void:
	var c: Color = MODEL_COLORS.get(model, Color(0.6, 0.5, 0.5))
	var body: MeshInstance3D = MeshInstance3D.new()
	var capsule: CapsuleMesh = CapsuleMesh.new()
	if LOW_MODELS.has(model):
		capsule.radius = 0.45 * scale
		capsule.height = 1.8 * scale
		body.rotation.x = PI / 2.0
		body.position = Vector3(0.0, 0.5 * scale, 0.0)
	else:
		capsule.radius = 0.4 * scale
		capsule.height = 1.8 * scale
		body.position = Vector3(0.0, 0.9 * scale, 0.0)
	body.mesh = capsule
	body.material_override = _mat(c)
	parent.add_child(body)
	# Eyes mark the front.
	var eye_mat: StandardMaterial3D = _mat(Color(1.0, 0.9, 0.3))
	eye_mat.emission_enabled = true
	eye_mat.emission = Color(1.0, 0.6, 0.1)
	var head_y: float = (0.7 if LOW_MODELS.has(model) else 1.5) * scale
	var head_z: float = (-0.85 if LOW_MODELS.has(model) else -0.33) * scale
	for side: float in [-1.0, 1.0]:
		var eye: MeshInstance3D = MeshInstance3D.new()
		var s: SphereMesh = SphereMesh.new()
		s.radius = 0.07 * scale
		s.height = 0.14 * scale
		eye.mesh = s
		eye.material_override = eye_mat
		eye.position = Vector3(side * 0.15 * scale, head_y, head_z)
		parent.add_child(eye)


static func build_corpse(parent: Node3D, model: String, scale: float) -> void:
	var c: Color = MODEL_COLORS.get(model, Color(0.6, 0.55, 0.5)).darkened(0.45)
	var body: MeshInstance3D = MeshInstance3D.new()
	var capsule: CapsuleMesh = CapsuleMesh.new()
	capsule.radius = 0.4 * scale
	capsule.height = 1.8 * scale
	body.mesh = capsule
	body.rotation = Vector3(PI / 2.0, 0.0, 0.25)
	body.position = Vector3(0.0, 0.35 * scale, 0.0)
	body.material_override = _mat(c)
	parent.add_child(body)


## A robed figure with a glowing sigil for NPCs (trainers).
static func build_npc(parent: Node3D) -> void:
	build(parent, Color(0.35, 0.3, 0.55), "", Color.WHITE)
	var robe: MeshInstance3D = MeshInstance3D.new()
	var cone: CylinderMesh = CylinderMesh.new()
	cone.top_radius = 0.42
	cone.bottom_radius = 0.65
	cone.height = 1.0
	robe.mesh = cone
	robe.position = Vector3(0.0, 0.5, 0.0)
	robe.material_override = _mat(Color(0.25, 0.2, 0.45))
	parent.add_child(robe)
	var sigil: MeshInstance3D = MeshInstance3D.new()
	var s: SphereMesh = SphereMesh.new()
	s.radius = 0.12
	s.height = 0.24
	sigil.mesh = s
	var m: StandardMaterial3D = _mat(Color(0.7, 1.0, 0.8))
	m.emission_enabled = true
	m.emission = Color(0.5, 1.0, 0.7)
	m.emission_energy_multiplier = 2.0
	sigil.material_override = m
	sigil.position = Vector3(0.0, 2.05, 0.0)
	parent.add_child(sigil)
