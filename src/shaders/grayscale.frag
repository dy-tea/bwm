precision mediump float;
uniform sampler2D tex;
varying vec2 v_uv;

void main() {
  vec3 c = texture2D(tex, v_uv).rgb;
  float g = dot(c, vec3(0.2126, 0.7152, 0.0722));
  gl_FragColor = vec4(g, g, g, 1.0);
}
