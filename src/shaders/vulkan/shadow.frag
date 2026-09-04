#version 450
layout(binding = 0) uniform sampler2D _dummy;
layout(push_constant) uniform PC {
  vec2 resolution;
  float shadow_size;
  vec4 shadow_color;
  float border_radius;
  vec2 inner_size;
  vec2 hole_pos;
  vec2 hole_size;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

float sdRoundedBox(vec2 p, vec2 b, float r) {
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
  vec2 px = v_uv * pc.resolution;
  vec2 half_inner = pc.inner_size * 0.5;
  vec2 center = pc.resolution * 0.5;
  float d_shadow = sdRoundedBox(px - center, half_inner, pc.border_radius);
  vec2 half_hole = pc.hole_size * 0.5;
  vec2 hole_center = pc.hole_pos + half_hole;
  float d_hole = sdRoundedBox(px - hole_center, half_hole, pc.border_radius);
  float alpha = 1.0 - smoothstep(0.0, pc.shadow_size, d_shadow);
  float fw = fwidth(d_hole);
  float hole = smoothstep(-fw * 0.5, fw * 0.5, d_hole);
  alpha *= hole * pc.shadow_color.a;
  if (alpha < 0.003) discard;
  fragColor = vec4(pc.shadow_color.rgb * alpha, alpha);
}
