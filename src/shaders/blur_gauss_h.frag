#include "color_helpers.glsl"

precision mediump float;
uniform sampler2D tex;
uniform vec2 texel_size;
uniform float radius;
uniform float vibrancy;
uniform float vibrancy_darkness;
varying vec2 v_uv;

float gauss(mediump float x, mediump float s) {
	return exp(-(x*x)/(2.0*s*s));
}

void main() {
	float sigma = max(radius / 3.0, 1.0);
	vec4 color = vec4(0.0);
	float total = 0.0;
	for (float i = -radius; i <= radius; i += 1.0) {
		float w = gauss(i, sigma);
		color += texture2D(tex, v_uv + vec2(i * texel_size.x, 0.0)) * w;
		total += w;
	}
	color = color / total;

	if (vibrancy > 0.0) {
		vec3 hsl = rgb2hsl(color.rgb);
		float brightness = getPerceivedBrightness(color.rgb);

		float boost = brightness * vibrancy * vibrancy_darkness;
		hsl.y = clamp(hsl.y + (boost * 0.15), 0.0, 1.0);

		color.rgb = hsl2rgb(hsl);
	}

	gl_FragColor = color;
}
