precision mediump float;
uniform sampler2D tex;
uniform vec4 tint;
uniform float tint_strength;
varying vec2 v_uv;

void main() {
  vec3 base = texture2D(tex, v_uv).rgb;
  gl_FragColor = vec4(mix(base, tint.rgb, tint_strength), 1.0);
}
