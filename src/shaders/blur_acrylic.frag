precision mediump float;
uniform sampler2D tex;
uniform vec4 tint;
uniform float tint_strength;
uniform float noise_strength;
uniform vec2 resolution;
uniform vec2 light_anchor;
varying vec2 v_uv;

vec3 hash3(vec2 p) {
  vec3 q = vec3(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)), dot(p, vec2(419.2, 371.9)));
  return fract(sin(q) * 43758.5453);
}

void main() {
  vec3 base = texture2D(tex, v_uv).rgb;
  vec3 tinted = mix(base, tint.rgb, clamp(tint_strength, 0.0, 0.35));
  tinted = pow(tinted, vec3(0.92));
  vec2 px = floor(v_uv * resolution);
  vec3 grain = (hash3(px) * 2.0 - 1.0) * 0.5 + 0.5;
  float highlight = pow(max(0.0, 1.0 - length(v_uv - light_anchor)), 6.0);
  vec3 spec = vec3(1.0) * highlight * 0.15;
  vec3 rim = vec3(0.08) * smoothstep(0.75, 1.0, length(v_uv - 0.5));
  vec3 color = tinted + spec + rim;
  color += (grain - 0.5) * noise_strength * 0.25;
  gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
