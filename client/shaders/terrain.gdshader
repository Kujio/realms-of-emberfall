// Outdoor ground (Phase 11): up to four texture layers blended by height, slope and nearness to water,
// each a detail map (value / 0.5 is a factor) over a colour from the content. World-space UVs, one
// large-scale variation map against visible tiling. Set up by scripts/world/surfaces.gd.
shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

uniform sampler2D base_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform sampler2D high_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform sampler2D slope_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform sampler2D shore_tex : repeat_enable, filter_linear_mipmap_anisotropic;
uniform sampler2D macro_tex : repeat_enable, filter_linear_mipmap;
uniform vec3 base_tint : source_color = vec3(0.3, 0.4, 0.2);
uniform vec3 high_tint : source_color = vec3(0.4, 0.38, 0.35);
uniform vec3 slope_tint : source_color = vec3(0.4, 0.38, 0.35);
uniform vec3 shore_tint : source_color = vec3(0.25, 0.22, 0.17);
// Metres per texture tile.
uniform float scale = 5.0;
// The high layer fades in with height above base_y, as a fraction of top.
uniform float base_y = 0.0;
uniform float top = 10.0;
// Water: the shore layer and wet darkening below level + band. A level far below disables both.
uniform float water_level = -10000.0;
uniform float shore_band = 0.8;

varying vec3 world_pos;
varying vec3 world_normal;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

// A detail sample as a factor around 1 (divided by the texture's own average, its smallest mip, so
// near and far ground match exactly), flattened with distance: far off, a repeated tile only draws
// a grid, so the large-scale variation takes over there.
vec3 detail(sampler2D tex, vec3 t, float fade) {
	vec3 avg = max(textureLod(tex, vec2(0.5), 12.0).rgb, vec3(0.05));
	return mix(t / avg, vec3(1.0), fade);
}

void fragment() {
	vec2 uv = world_pos.xz / scale;
	vec3 m = texture(macro_tex, world_pos.xz / 190.0).rgb;
	vec3 m2 = texture(macro_tex, world_pos.xz / 41.0 + vec2(0.23, 0.71)).rgb;
	float fade = smoothstep(30.0, 180.0, distance(world_pos, CAMERA_POSITION_WORLD)) * 0.7;
	// The base layer at two scales, so its repeats do not line up.
	vec3 base = mix(texture(base_tex, uv).rgb, texture(base_tex, uv * 0.29 + vec2(0.37, 0.61)).rgb, 0.4);
	vec3 c = base_tint * detail(base_tex, base, fade);
	// Most ground carries only the base layer: the others are read only where they show (explicit
	// gradients, since a texture read inside a branch has no reliable implicit ones).
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float h = clamp((world_pos.y - base_y) / max(top, 0.01), 0.0, 1.0);
	float w_high = smoothstep(0.35, 0.8, h + (m.r - 0.5) * 0.35);
	if (w_high > 0.002) {
		c = mix(c, high_tint * detail(high_tex, textureGrad(high_tex, uv, dx, dy).rgb, fade), w_high);
	}
	float slope = 1.0 - clamp(world_normal.y, 0.0, 1.0);
	float w_slope = smoothstep(0.16, 0.32, slope + (m.g - 0.5) * 0.12);
	if (w_slope > 0.002) {
		c = mix(c, slope_tint * detail(slope_tex, textureGrad(slope_tex, uv * 0.7, dx * 0.7, dy * 0.7).rgb, fade), w_slope);
	}
	float y = world_pos.y + (m2.r - 0.5) * 0.5;
	float w_shore = 1.0 - smoothstep(water_level + 0.05, water_level + shore_band, y);
	if (w_shore > 0.002) {
		c = mix(c, shore_tint * detail(shore_tex, textureGrad(shore_tex, uv, dx, dy).rgb, fade), w_shore);
	}
	// Light and dark patches at two scales, bigger than any tile, hide the tiling.
	c *= (0.82 + 0.36 * m.b) * (0.88 + 0.24 * m2.g);
	float wet = 1.0 - smoothstep(water_level - 0.05, water_level + 0.25, world_pos.y);
	ALBEDO = c * (1.0 - 0.35 * wet);
	ROUGHNESS = mix(0.95, 0.35, wet);
	SPECULAR = 0.25;
}
