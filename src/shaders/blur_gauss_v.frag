#include "color_helpers.glsl"

precision mediump float;
uniform sampler2D tex;
uniform vec2 texel_size;
uniform float radius;
uniform float vibrancy;
uniform float vibrancy_darkness;
uniform float brightness;
uniform float contrast;
varying vec2 v_uv;

float gauss(float x, float s) {
  return exp(-(x * x) / (2.0 * s * s));
}

void main() {
  float sigma = max(radius / 3.0, 1.0);
  vec4 color = vec4(0.0);
  float total = 0.0;
  for (float i = -radius; i <= radius; i += 1.0) {
    float w = gauss(i, sigma);
    color += texture2D(tex, v_uv + vec2(0.0, i * texel_size.y)) * w;
    total += w;
  }
  color = color / total;

  if (vibrancy > 0.0) {
    color.rgb = applyVibrancy(color.rgb, vibrancy, vibrancy_darkness, 2.0);
  }

  color.rgb = applyBrightnessContrast(color.rgb, brightness, contrast);

  gl_FragColor = color;
}
