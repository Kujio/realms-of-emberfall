// Water (Phase 11): a flat transparent sheet at the zone's water level. Two scrolling copies of one
// normal map make the ripples; seen edge-on it turns opaque (Fresnel). The Compatibility renderer
// gives no depth read here, so the shore band is drawn by the terrain shader instead.
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_disabled, diffuse_burley, specular_schlick_ggx;

uniform vec3 color : source_color = vec3(0.12, 0.2, 0.22);
// How much of the ground is hidden when looking straight down (0 clear, 1 opaque).
uniform float opacity : hint_range(0.0, 1.0) = 0.7;
uniform sampler2D normal_tex : hint_normal, repeat_enable, filter_linear_mipmap;
uniform float scale = 12.0;
uniform vec2 flow = vec2(0.015, 0.008);
uniform float ripple = 0.5;

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 uv = world_pos.xz / scale;
	// The map is imported as a two-channel normal map (RGTC): only red and green survive, so each
	// normal's Z is rebuilt before the two are blended.
	vec2 a = texture(normal_tex, uv + TIME * flow).rg * 2.0 - 1.0;
	vec2 b = texture(normal_tex, uv * 1.7 + vec2(0.5, 0.25) - TIME * flow * 1.3).rg * 2.0 - 1.0;
	vec3 n1 = vec3(a, sqrt(max(0.0, 1.0 - dot(a, a))));
	vec3 n2 = vec3(b, sqrt(max(0.0, 1.0 - dot(b, b))));
	NORMAL_MAP = normalize(n1 + n2) * 0.5 + 0.5;
	NORMAL_MAP_DEPTH = ripple;
	float facing = clamp(dot(NORMAL, VIEW), 0.0, 1.0);
	float fresnel = pow(1.0 - facing, 4.0);
	// Low specular: the sky's reflection would otherwise wash every pond out to the horizon's white.
	ALBEDO = color * (1.0 - 0.35 * fresnel);
	ROUGHNESS = 0.2;
	SPECULAR = 0.12;
	ALPHA = clamp(mix(opacity, 1.0, fresnel), 0.0, 1.0);
}
