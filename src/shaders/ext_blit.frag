#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES tex;
varying vec2 v_uv;

void main() {
	gl_FragColor = vec4(texture2D(tex, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0);
}
