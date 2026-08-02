precision highp float;
uniform sampler2D tex;
uniform vec2 halfpixel;
uniform float offset;
varying vec2 v_uv;

void main() {
  vec2 o = halfpixel * offset;

  vec4 sum = texture2D(tex, v_uv) * 4.0;
  sum += texture2D(tex, v_uv + vec2(-o.x, -o.y));
  sum += texture2D(tex, v_uv + vec2( o.x, -o.y));
  sum += texture2D(tex, v_uv + vec2(-o.x,  o.y));
  sum += texture2D(tex, v_uv + vec2( o.x,  o.y));

  gl_FragColor = sum / 8.0;
}
