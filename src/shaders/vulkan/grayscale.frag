#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
  vec3 c = texture(tex, v_uv).rgb;
  float g = dot(c, vec3(0.2126, 0.7152, 0.0722));
  fragColor = vec4(g, g, g, 1.0);
}
