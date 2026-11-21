## Placeholder character visuals: a capsule body, a "nose" showing facing, and a name tag.
extends RefCounted


static func color_for_class(class_id: String) -> Color:
	var h: int = absi(class_id.hash())
	return Color.from_hsv(float(h % 360) / 360.0, 0.55, 0.85)


static func build(parent: Node3D, body_color: Color, label_text: String, label_color: Color) -> void:
	var body_mat: StandardMaterial3D = StandardMaterial3D.new()
	body_mat.albedo_color = body_color
	body_mat.roughness = 0.8
	var body: MeshInstance3D = MeshInstance3D.new()
	var capsule: CapsuleMesh = CapsuleMesh.new()
	capsule.radius = 0.4
	capsule.height = 1.8
	body.mesh = capsule
	body.position = Vector3(0.0, 0.9, 0.0)
	body.material_override = body_mat
	parent.add_child(body)

	var nose_mat: StandardMaterial3D = StandardMaterial3D.new()
	nose_mat.albedo_color = body_color.darkened(0.6)
	var nose: MeshInstance3D = MeshInstance3D.new()
	var box: BoxMesh = BoxMesh.new()
	box.size = Vector3(0.22, 0.18, 0.4)
	nose.mesh = box
	nose.position = Vector3(0.0, 1.45, -0.38)
	nose.material_override = nose_mat
	parent.add_child(nose)

	var tag: Label3D = Label3D.new()
	tag.text = label_text
	tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	tag.position = Vector3(0.0, 2.25, 0.0)
	tag.font_size = 40
	tag.outline_size = 10
	tag.modulate = label_color
	tag.no_depth_test = true
	parent.add_child(tag)
