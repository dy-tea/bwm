precision mediump float;
uniform sampler2D tex;
uniform vec2 halfpixel;
uniform float offset;
uniform float noise_strength;
varying vec2 v_uv;

// Simple hash function for noise generation
float hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

void main() {
  vec2 uv = v_uv;
  vec4 s = texture2D(tex, uv) * 4.0;
  s += texture2D(tex, uv - halfpixel * offset);
  s += texture2D(tex, uv + halfpixel * offset);
  s += texture2D(tex, uv + vec2(halfpixel.x, -halfpixel.y) * offset);
  s += texture2D(tex, uv + vec2(-halfpixel.x, halfpixel.y) * offset);
  vec4 color = s / 8.0;

  if (noise_strength > 0.0) {
    float noiseVal = hash(v_uv) - 0.5;
    color.rgb += noiseVal * noise_strength * 0.5;
  }

  gl_FragColor = color;
}
