// Sky (Phase 11): a gradient from the zone's sky colour to its horizon, a sun disc and glow from the
// sun light, and a layer of clouds from a tileable density map projected on a dome. No TIME: a sky
// that changes every frame re-renders its radiance every frame, which integrated graphics cannot
// spare, so the clouds hold still.
shader_type sky;

uniform vec3 top_color : source_color = vec3(0.29, 0.39, 0.6);
uniform vec3 horizon_color : source_color = vec3(0.78, 0.63, 0.52);
uniform vec3 ground_color : source_color = vec3(0.2, 0.17, 0.15);
uniform sampler2D clouds_tex : repeat_enable, filter_linear_mipmap;
// 0 = clear, 1 = overcast.
uniform float cover : hint_range(0.0, 1.0) = 0.35;
uniform vec3 cloud_color : source_color = vec3(1.0, 0.98, 0.95);
uniform vec3 cloud_shade : source_color = vec3(0.6, 0.62, 0.68);
uniform float sun_size = 1.0;

void sky() {
	vec3 d = EYEDIR;
	float up = clamp(d.y, 0.0, 1.0);
	vec3 col = mix(horizon_color, top_color, pow(up, 0.55));
	if (d.y < 0.0) {
		col = mix(horizon_color, ground_color, clamp(-d.y * 5.0, 0.0, 1.0));
	}
	float glow = 0.0;
	if (LIGHT0_ENABLED) {
		float c = max(dot(d, LIGHT0_DIRECTION), 0.0);
		float disc = smoothstep(1.0 - 0.0006 * sun_size, 1.0 - 0.0003 * sun_size, c);
		glow = pow(c, 400.0) * 0.6 + pow(c, 24.0) * 0.18;
		col += LIGHT0_COLOR * (disc * 6.0 + glow) * LIGHT0_ENERGY;
	}
	if (d.y > 0.0 && cover > 0.0) {
		vec2 uv = d.xz / (d.y + 0.14) * 0.16;
		vec3 a = texture(clouds_tex, uv).rgb;
		vec3 b = texture(clouds_tex, uv * 2.7 + vec2(0.31, 0.17)).rgb;
		float dens = mix(a.r, a.g, 0.35) * 0.75 + b.r * 0.25;
		float edge = 1.0 - cover;
		float amount = smoothstep(edge - 0.08, edge + 0.22, dens);
		// High haze thickens toward overcast, so heavy cover reads as a grey lid, not blobs.
		amount = max(amount, smoothstep(0.55, 1.0, cover) * (0.55 + 0.45 * a.b));
		amount *= smoothstep(0.0, 0.18, d.y);
		vec3 cc = mix(cloud_shade, cloud_color, clamp((dens - edge) * 2.5 + 0.35, 0.0, 1.0));
		if (LIGHT0_ENABLED) {
			cc += LIGHT0_COLOR * glow * 0.6;
		}
		col = mix(col, cc, amount * 0.92);
	}
	COLOR = col;
}
