#include "color_helpers.glsl"

precision mediump float;
uniform sampler2D tex;
uniform vec2 texel_size;
uniform float radius;
uniform float vibrancy;
uniform float vibrancy_darkness;
varying vec2 v_uv;

void main() {
	float r = floor(radius);
	float count = 2.0 * r + 1.0;
	vec4 color = vec4(0.0);
	for (float i = -r; i <= r; i += 1.0)
		color += texture2D(tex, v_uv + vec2(i * texel_size.x, 0.0));
	color = color / count;

	if (vibrancy > 0.0) {
		vec3 hsl = rgb2hsl(color.rgb);
		float brightness = getPerceivedBrightness(color.rgb);

		float boost = brightness * vibrancy * vibrancy_darkness;
		hsl.y = clamp(hsl.y + (boost * 0.15), 0.0, 1.0);

		color.rgb = hsl2rgb(hsl);
	}

	gl_FragColor = color;
}
