#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
  vec3 c = texture(tex, v_uv).rgb;
  vec3 s;
  s.r = dot(c, vec3(0.393, 0.769, 0.189));
  s.g = dot(c, vec3(0.349, 0.686, 0.168));
  s.b = dot(c, vec3(0.272, 0.534, 0.131));
  fragColor = vec4(clamp(s, 0.0, 1.0), 1.0);
}
