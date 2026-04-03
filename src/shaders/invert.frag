precision mediump float;
uniform sampler2D tex;
varying vec2 v_uv;

void main() {\n"
  vec3 c = texture2D(tex, v_uv).rgb;
  gl_FragColor = vec4(1.0 - c, 1.0);
}
