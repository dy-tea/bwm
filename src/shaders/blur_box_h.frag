precision mediump float;
uniform sampler2D tex;
uniform vec2 texel_size;
uniform float radius;
varying vec2 v_uv;

void main() {
  float r = floor(radius);
  float count = 2.0 * r + 1.0;
  vec4 color = vec4(0.0);
  for (float i = -r; i <= r; i += 1.0)
    color += texture2D(tex, v_uv + vec2(i * texel_size.x, 0.0));
  gl_FragColor = color / count;
}
