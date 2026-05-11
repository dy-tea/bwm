#extension GL_OES_standard_derivatives : enable
precision mediump float;
uniform vec2 resolution;
uniform float border_radius;
uniform float border_width_px;
uniform vec4 border_color;
varying vec2 v_uv;

float sdRoundedBox(mediump vec2 p, mediump vec2 b, mediump float r) {
  mediump vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
  mediump vec2 px = v_uv * resolution;
  mediump vec2 center = resolution * 0.5;
  mediump vec2 p = px - center;
  mediump float d_outer = sdRoundedBox(p, center, border_radius);
  mediump float inner_r = max(border_radius - border_width_px, 0.0);
  mediump vec2 inner_half = center - vec2(border_width_px);
  mediump float d_inner = sdRoundedBox(p, inner_half, inner_r);
  mediump float fw_outer = max(length(vec2(dFdx(d_outer), dFdy(d_outer))), 0.5);
  mediump float a_outer = smoothstep(fw_outer * 0.5, -fw_outer * 0.5, d_outer);
  mediump float fw_inner = max(length(vec2(dFdx(d_inner), dFdy(d_inner))), 0.5);
  mediump float a_inner = smoothstep(-fw_inner, fw_inner * 0.25, d_inner);
  mediump float alpha = a_outer * a_inner * border_color.a;
  if (alpha < 0.001) discard;
  gl_FragColor = vec4(border_color.rgb * alpha, alpha);
}
