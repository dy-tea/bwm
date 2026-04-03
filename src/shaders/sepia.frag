precision mediump float;
uniform sampler2D tex;
varying vec2 v_uv;

void main() {
  vec3 c = texture2D(tex, v_uv).rgb;
  vec3 s;
  s.r = dot(c, vec3(0.393, 0.769, 0.189));
  s.g = dot(c, vec3(0.349, 0.686, 0.168));
  s.b = dot(c, vec3(0.272, 0.534, 0.131));
  gl_FragColor = vec4(clamp(s, 0.0, 1.0), 1.0);
}
