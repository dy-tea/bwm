#extension GL_OES_standard_derivatives : enable
precision highp float;
uniform sampler2D tex;
uniform vec2 win_pos_uv;
uniform vec2 win_size_uv;
uniform vec2 win_size_px;
uniform float border_radius_px;
uniform float scale;
uniform vec2 bg_pos_uv;
uniform vec2 bg_size_uv;
varying vec2 v_uv;

float rounding_alpha(vec2 coords, vec2 size, float radius) {
  if (radius <= 0.0) return 1.0;

  vec2 center;
  bool in_corner = false;

  if (coords.x < radius && coords.y < radius) {
    center = vec2(radius, radius);
    in_corner = true;
  } else if (size.x - radius < coords.x && coords.y < radius) {
    center = vec2(size.x - radius, radius);
    in_corner = true;
  } else if (size.x - radius < coords.x && size.y - radius < coords.y) {
    center = vec2(size.x - radius, size.y - radius);
    in_corner = true;
  } else if (coords.x < radius && size.y - radius < coords.y) {
    center = vec2(radius, size.y - radius);
    in_corner = true;
  }

  if (!in_corner) return 1.0;

  float dist = distance(coords, center);
  float t = clamp((dist - radius) * scale + 0.5, 0.0, 1.0);
  return 1.0 - t * t * (3.0 - 2.0 * t);
}

void main() {
  vec2 win_local = (v_uv - win_pos_uv) / win_size_uv;
  if (win_local.x < 0.0 || win_local.x > 1.0 || win_local.y < 0.0 || win_local.y > 1.0) {
    gl_FragColor = vec4(0.0);
    return;
  }
  vec2 coords = win_local * win_size_px;
  float alpha = rounding_alpha(coords, win_size_px, border_radius_px);
  vec2 bg_uv = bg_pos_uv + v_uv * bg_size_uv;
  vec4 bg = texture2D(tex, bg_uv);
  gl_FragColor = vec4(bg.rgb * alpha, alpha);
}
