// Dungeon walls and floors (Phase 11): the seeded layout's floor and wall blocks (MultiMesh boxes),
// textured by world position: the top face from top_tex, the side faces from side_tex, each a detail
// map over a theme colour. The instance colour shades each block a little.
shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

uniform sampler2D top_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform sampler2D side_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform vec3 top_tint : source_color = vec3(0.3, 0.27, 0.24);
uniform vec3 side_tint : source_color = vec3(0.35, 0.31, 0.29);
// Metres per texture tile.
uniform float top_scale = 4.0;
uniform float side_scale = 3.0;
uniform float roughness = 0.9;

varying vec3 world_pos;
varying vec3 world_normal;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
	vec3 an = abs(world_normal);
	vec3 c;
	if (an.y > 0.5) {
		c = top_tint * texture(top_tex, world_pos.xz / top_scale).rgb * 2.0;
	} else {
		vec2 uv = an.x > an.z ? vec2(world_pos.z, -world_pos.y) : vec2(world_pos.x, -world_pos.y);
		c = side_tint * texture(side_tex, uv / side_scale).rgb * 2.0;
	}
	ALBEDO = c * COLOR.rgb;
	ROUGHNESS = roughness;
	SPECULAR = 0.2;
}
