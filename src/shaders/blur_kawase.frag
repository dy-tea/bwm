precision mediump float;
uniform sampler2D tex;
uniform vec2 halfpixel;
uniform float offset;
varying vec2 v_uv;

void main() {
	vec2 uv = v_uv;
	vec4 s = texture2D(tex, uv) * 4.0;
	s += texture2D(tex, uv - halfpixel * offset);
	s += texture2D(tex, uv + halfpixel * offset);
	s += texture2D(tex, uv + vec2( halfpixel.x, -halfpixel.y) * offset);
	s += texture2D(tex, uv + vec2(-halfpixel.x,  halfpixel.y) * offset);
	gl_FragColor = s / 8.0;
}
