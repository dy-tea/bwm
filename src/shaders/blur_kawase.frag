#include "color_helpers.glsl"

precision mediump float;
uniform sampler2D tex;
uniform vec2 halfpixel;
uniform float offset;
uniform float noise_strength;
uniform float vibrancy;
uniform float vibrancy_darkness;
uniform float brightness;
uniform float contrast;
varying vec2 v_uv;

void main() {
  vec2 uv = v_uv;
  vec4 s = texture2D(tex, uv) * 4.0;
  s += texture2D(tex, uv - halfpixel * offset);
  s += texture2D(tex, uv + halfpixel * offset);
  s += texture2D(tex, uv + vec2(halfpixel.x, -halfpixel.y) * offset);
  s += texture2D(tex, uv + vec2(-halfpixel.x, halfpixel.y) * offset);
  vec4 color = s / 8.0;

  if (noise_strength > 0.0) {
    color.rgb = addNoise(color.rgb, v_uv, noise_strength * 0.5);
  }

  if (vibrancy > 0.0) {
    color.rgb = applyVibrancy(color.rgb, vibrancy, vibrancy_darkness, 1.0);
  }

  color.rgb = applyBrightnessContrast(color.rgb, brightness, contrast);

  gl_FragColor = color;
}
