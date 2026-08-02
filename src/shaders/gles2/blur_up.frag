#include "color_helpers.glsl"

precision highp float;
uniform sampler2D tex;
uniform vec2 halfpixel;
uniform float offset;
uniform float adjust;
uniform float saturation;
uniform float noise_strength;
uniform float vibrancy;
uniform float vibrancy_darkness;
uniform float brightness;
uniform float contrast;
varying vec2 v_uv;

void main() {
  vec2 o = halfpixel * offset;

  vec4 sum = vec4(0.0);
  sum += texture2D(tex, v_uv + vec2(-o.x * 2.0, 0.0));
  sum += texture2D(tex, v_uv + vec2( o.x * 2.0, 0.0));
  sum += texture2D(tex, v_uv + vec2(0.0, -o.y * 2.0));
  sum += texture2D(tex, v_uv + vec2(0.0,  o.y * 2.0));
  sum += texture2D(tex, v_uv + vec2(-o.x,  o.y)) * 2.0;
  sum += texture2D(tex, v_uv + vec2( o.x,  o.y)) * 2.0;
  sum += texture2D(tex, v_uv + vec2(-o.x, -o.y)) * 2.0;
  sum += texture2D(tex, v_uv + vec2( o.x, -o.y)) * 2.0;
  vec4 color = sum / 12.0;

  if (adjust > 0.5) {
    color.rgb = applySaturation(color.rgb, saturation);
    if (noise_strength > 0.0) {
      color.rgb = addNoise(color.rgb, v_uv, noise_strength * 0.5);
    }
    if (vibrancy > 0.0) {
      color.rgb = applyVibrancy(color.rgb, vibrancy, vibrancy_darkness, 1.0);
    }
    color.rgb = applyBrightnessContrast(color.rgb, brightness, contrast);
  }

  gl_FragColor = color;
}
