precision mediump float;
uniform sampler2D tex;
uniform vec2 texel_size;
uniform float radius;
varying vec2 v_uv;

float gauss(float x, float s) {
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
	gl_FragColor = color / total;
}
