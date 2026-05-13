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

void main() {
  float r = floor(radius);
  float count = 2.0 * r + 1.0;
  vec4 color = vec4(0.0);
  for (float i = -r; i <= r; i += 1.0)
    color += texture2D(tex, v_uv + vec2(i * texel_size.x, 0.0));
  color = color / count;

  if (vibrancy > 0.0) {
    color.rgb = applyVibrancy(color.rgb, vibrancy, vibrancy_darkness, 2.0);
  }

  color.rgb = applyBrightnessContrast(color.rgb, brightness, contrast);

  gl_FragColor = color;
}
