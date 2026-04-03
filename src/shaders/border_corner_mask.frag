#extension GL_OES_standard_derivatives : enable
precision highp float;
uniform sampler2D tex;
uniform vec2 win_pos_uv;
uniform vec2 win_size_uv;
uniform vec2 win_size_px;
uniform float border_radius_px;
varying vec2 v_uv;

float sdRoundedBox(vec2 p, vec2 b, float r) {
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
  vec2 win_local = (v_uv - win_pos_uv) / win_size_uv;
  if (win_local.x < 0.0 || win_local.x > 1.0 || win_local.y < 0.0 || win_local.y > 1.0) {
    gl_FragColor = vec4(0.0);
    return;
  }
  vec2 p_px = (win_local - 0.5) * win_size_px;
  vec2 half_px = win_size_px * 0.5;
  float d = sdRoundedBox(p_px, half_px, border_radius_px);
  float fw = max(length(vec2(dFdx(d), dFdy(d))), 0.5);
  float alpha = smoothstep(-fw * 0.15, fw * 0.85, d);
  if (alpha < 0.001) {
    gl_FragColor = vec4(0.0);
    return;
  }
  vec4 bg = texture2D(tex, v_uv);
  gl_FragColor = vec4(bg.rgb * alpha, bg.a * alpha);
}
