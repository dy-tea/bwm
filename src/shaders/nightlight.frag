precision mediump float;
uniform sampler2D tex;
varying vec2 v_uv;

void main() {
  vec3 c = texture2D(tex, v_uv).rgb;
  c.r = min(c.r * 1.05, 1.0);
  c.g = c.g * 0.92;
  c.b = c.b * 0.75;
  gl_FragColor = vec4(c, 1.0);
}
