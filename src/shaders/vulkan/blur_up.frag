#version 450
#include "color_helpers.glsl"

layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
  vec2 halfpixel;
  float offset;
  float adjust;
  float saturation;
  float vibrancy;
  float vibrancy_darkness;
  float brightness;
  float contrast;
  float noise_strength;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main() {
  vec2 o = pc.halfpixel * pc.offset;

  vec4 sum = vec4(0.0);
  sum += texture(tex, v_uv + vec2(-o.x * 2.0, 0.0));
  sum += texture(tex, v_uv + vec2( o.x * 2.0, 0.0));
  sum += texture(tex, v_uv + vec2(0.0, -o.y * 2.0));
  sum += texture(tex, v_uv + vec2(0.0,  o.y * 2.0));
  sum += texture(tex, v_uv + vec2(-o.x,  o.y)) * 2.0;
  sum += texture(tex, v_uv + vec2( o.x,  o.y)) * 2.0;
  sum += texture(tex, v_uv + vec2(-o.x, -o.y)) * 2.0;
  sum += texture(tex, v_uv + vec2( o.x, -o.y)) * 2.0;
  vec4 color = sum / 12.0;

  if (pc.adjust > 0.5) {
    color.rgb = applySaturation(color.rgb, pc.saturation);
    if (pc.noise_strength > 0.0)
      color.rgb = addNoise(color.rgb, v_uv, pc.noise_strength * 0.5);
    if (pc.vibrancy > 0.0)
      color.rgb = applyVibrancy(color.rgb, pc.vibrancy, pc.vibrancy_darkness, 1.0);
    color.rgb = applyBrightnessContrast(color.rgb, pc.brightness, pc.contrast);
  }

  fragColor = color;
}
