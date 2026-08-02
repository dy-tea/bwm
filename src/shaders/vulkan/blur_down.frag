#version 450

layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
  vec2 halfpixel;
  float offset;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main() {
  vec2 o = pc.halfpixel * pc.offset;

  vec4 sum = texture(tex, v_uv) * 4.0;
  sum += texture(tex, v_uv + vec2(-o.x, -o.y));
  sum += texture(tex, v_uv + vec2( o.x, -o.y));
  sum += texture(tex, v_uv + vec2(-o.x,  o.y));
  sum += texture(tex, v_uv + vec2( o.x,  o.y));

  fragColor = sum / 8.0;
}
