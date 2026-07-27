#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
  vec3 c = texture(tex, v_uv).rgb;
  fragColor = vec4(1.0 - c, 1.0);
}
