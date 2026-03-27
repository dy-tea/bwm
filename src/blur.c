#include "blur.h"
#include "server.h"
#include "output.h"
#include "toplevel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <wlr/util/log.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <drm_fourcc.h>

bool blur_enabled = true;
enum blur_algorithm blur_algorithm = BLUR_ALGORITHM_KAWASE;
int blur_passes = 1;
float blur_radius = 5.0f;
int blur_downsample = 4;
bool mica_enabled = false;
float mica_tint[4] = {0.12f, 0.12f, 0.14f, 1.0f};
float mica_tint_strength = 0.35f;

struct bwm_blur_ctx blur_ctx = {0};

static const char *vert_src =
  "attribute vec2 pos;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "    v_uv = pos * 0.5 + 0.5;\n"
  "    gl_Position = vec4(pos, 0.0, 1.0);\n"
  "}\n";

static const char *frag_kawase_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "uniform vec2 halfpixel;\n"
  "uniform float offset;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "    vec2 uv = v_uv;\n"
  "    vec4 s = texture2D(tex, uv) * 4.0;\n"
  "    s += texture2D(tex, uv - halfpixel * offset);\n"
  "    s += texture2D(tex, uv + halfpixel * offset);\n"
  "    s += texture2D(tex, uv + vec2( halfpixel.x, -halfpixel.y) * offset);\n"
  "    s += texture2D(tex, uv + vec2(-halfpixel.x,  halfpixel.y) * offset);\n"
  "    gl_FragColor = s / 8.0;\n"
  "}\n";

static const char *frag_gauss_h_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "uniform vec2 texel_size;\n"
  "uniform float radius;\n"
  "varying vec2 v_uv;\n"
  "float gauss(float x, float s) { return exp(-(x*x)/(2.0*s*s)); }\n"
  "void main() {\n"
  "    float sigma = max(radius / 3.0, 1.0);\n"
  "    vec4  color = vec4(0.0); float total = 0.0;\n"
  "    for (float i = -radius; i <= radius; i += 1.0) {\n"
  "        float w = gauss(i, sigma);\n"
  "        color += texture2D(tex, v_uv + vec2(i * texel_size.x, 0.0)) * w;\n"
  "        total += w;\n"
  "    }\n"
  "    gl_FragColor = color / total;\n"
  "}\n";

static const char *frag_gauss_v_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "uniform vec2 texel_size;\n"
  "uniform float radius;\n"
  "varying vec2 v_uv;\n"
  "float gauss(float x, float s) { return exp(-(x*x)/(2.0*s*s)); }\n"
  "void main() {\n"
  "    float sigma = max(radius / 3.0, 1.0);\n"
  "    vec4  color = vec4(0.0); float total = 0.0;\n"
  "    for (float i = -radius; i <= radius; i += 1.0) {\n"
  "        float w = gauss(i, sigma);\n"
  "        color += texture2D(tex, v_uv + vec2(0.0, i * texel_size.y)) * w;\n"
  "        total += w;\n"
  "    }\n"
  "    gl_FragColor = color / total;\n"
  "}\n";

static const char *frag_box_h_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "uniform vec2 texel_size;\n"
  "uniform float radius;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "    float r = floor(radius);\n"
  "    float count = 2.0 * r + 1.0;\n"
  "    vec4 color = vec4(0.0);\n"
  "    for (float i = -r; i <= r; i += 1.0)\n"
  "        color += texture2D(tex, v_uv + vec2(i * texel_size.x, 0.0));\n"
  "    gl_FragColor = color / count;\n"
  "}\n";

static const char *frag_box_v_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "uniform vec2 texel_size;\n"
  "uniform float radius;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "    float r = floor(radius);\n"
  "    float count = 2.0 * r + 1.0;\n"
  "    vec4 color = vec4(0.0);\n"
  "    for (float i = -r; i <= r; i += 1.0)\n"
  "        color += texture2D(tex, v_uv + vec2(0.0, i * texel_size.y));\n"
  "    gl_FragColor = color / count;\n"
  "}\n";

static const char *frag_blit_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "varying vec2 v_uv;\n"
  "void main() { gl_FragColor = vec4(texture2D(tex, v_uv).rgb, 1.0); }\n";

static const char *frag_ext_blit_src =
  "#extension GL_OES_EGL_image_external : require\n"
  "precision mediump float;\n"
  "uniform samplerExternalOES tex;\n"
  "varying vec2 v_uv;\n"
  "void main() { gl_FragColor = vec4(texture2D(tex, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }\n";

static const char *frag_mica_tint_src =
  "precision mediump float;\n"
  "uniform sampler2D tex;\n"
  "uniform vec4  tint;\n"
  "uniform float tint_strength;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "  vec3 base = texture2D(tex, v_uv).rgb;\n"
  "  gl_FragColor = vec4(mix(base, tint.rgb, tint_strength), 1.0);\n"
  "}\n";

static EGLDisplay s_egl_display = EGL_NO_DISPLAY;
static EGLContext s_egl_context = EGL_NO_CONTEXT;

static bool egl_make_current(void) {
  return eglMakeCurrent(s_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_egl_context) == EGL_TRUE;
}

static void egl_unset_current(void) {
  eglMakeCurrent(s_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static const struct wlr_backend_impl capture_backend_impl = {0};

static bool capture_output_test(struct wlr_output *output, const struct wlr_output_state *state) {
	(void)output;
  uint32_t supported = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
    WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;
  return (state->committed & ~supported) == 0;
}

static bool capture_output_commit(struct wlr_output *output, const struct wlr_output_state *state) {
  (void)output;
  (void)state;
  return true;
}

static const struct wlr_output_impl capture_output_impl = {
  .test = capture_output_test,
  .commit = capture_output_commit,
};

static size_t capture_output_num = 0;

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

static bool create_capture_output(struct bwm_blur_output_ctx *ctx, int width, int height) {
  (void)width;
  (void)height;

  // backend
  ctx->capture_backend = calloc(1, sizeof(struct wlr_backend));
  if (!ctx->capture_backend) return false;
  wlr_backend_init(ctx->capture_backend, &capture_backend_impl);
  ctx->capture_backend->buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

  // output
  ctx->capture_output = calloc(1, sizeof(struct wlr_output));
  if (!ctx->capture_output) {
    wlr_backend_finish(ctx->capture_backend);
    free(ctx->capture_backend);
    ctx->capture_backend = NULL;
    return false;
  }

  struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
  wlr_output_init(ctx->capture_output, ctx->capture_backend,
    &capture_output_impl, loop, NULL);

  char name[64];
  snprintf(name, sizeof(name), "BLUR-%zu", ++capture_output_num);
  wlr_output_set_name(ctx->capture_output, name);

  wlr_output_init_render(ctx->capture_output,  server.allocator, server.renderer);

  // scene output, parked off-screen so surfaces don't become associated with it
  // while we're not actively capturing
  ctx->capture_scene_output = wlr_scene_output_create(server.scene, ctx->capture_output);
  if (!ctx->capture_scene_output) {
    wlr_output_finish(ctx->capture_output);
    free(ctx->capture_output);
    wlr_backend_finish(ctx->capture_backend);
    free(ctx->capture_backend);
    ctx->capture_output = NULL;
    ctx->capture_backend = NULL;
    return false;
  }

  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);
  wlr_log(WLR_INFO, "blur: created capture output %s", name);
  return true;
}

static void destroy_capture_output(struct bwm_blur_output_ctx *ctx) {
  if (ctx->capture_scene_output) {
    wlr_scene_output_destroy(ctx->capture_scene_output);
    ctx->capture_scene_output = NULL;
  }
  if (ctx->capture_output) {
    wlr_output_finish(ctx->capture_output);
    free(ctx->capture_output);
    ctx->capture_output = NULL;
  }
  if (ctx->capture_backend) {
    wlr_backend_finish(ctx->capture_backend);
    free(ctx->capture_backend);
    ctx->capture_backend = NULL;
  }
}

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    wlr_log(WLR_ERROR, "blur: shader compile error: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint link_program(const char *frag_src) {
  GLuint vert = compile_shader(GL_VERTEX_SHADER,   vert_src);
  GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
  if (!vert || !frag) {
    glDeleteShader(vert);
    glDeleteShader(frag);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glBindAttribLocation(prog, 0, "pos");
  glLinkProgram(prog);
  glDeleteShader(vert);
  glDeleteShader(frag);
  GLint ok;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(prog, sizeof(log), NULL, log);
    wlr_log(WLR_ERROR, "blur: program link error: %s", log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

static bool create_fbo(int w, int h, GLuint *fbo_out, GLuint *tex_out) {
  GLuint tex, fbo;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
     GL_TEXTURE_2D, tex, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR, "blur: FBO incomplete (0x%x)", status);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
    return false;
  }
  *fbo_out = fbo;
  *tex_out = tex;
  return true;
}

static void destroy_fbo(GLuint *fbo, GLuint *tex) {
  if (*fbo) {
    glDeleteFramebuffers(1, fbo);
    *fbo = 0;
  }
  if (*tex) {
    glDeleteTextures(1, tex);
    *tex = 0;
  }
}

static void draw_quad(void) {
  glBindBuffer(GL_ARRAY_BUFFER, blur_ctx.vbo);
  glEnableVertexAttribArray(blur_ctx.attr_pos);
  glVertexAttribPointer(blur_ctx.attr_pos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(blur_ctx.attr_pos);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void blur_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, int pass_index) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);

  glUseProgram(blur_ctx.prog_kawase);
  glUniform1i(blur_ctx.u_kawase.tex, 0);
  glUniform2f(blur_ctx.u_kawase.halfpixel, 0.5f / (float)w, 0.5f / (float)h);
  glUniform1f(blur_ctx.u_kawase.offset, (float)(pass_index + 1));

  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void box_pass(GLuint src_tex, GLuint ping_fbo, GLuint ping_tex,
    GLuint pong_fbo, int w, int h) {
  // H: src_tex -> ping_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, ping_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glUseProgram(blur_ctx.prog_box_h);
  glUniform1i(blur_ctx.u_box.tex, 0);
  glUniform2f(blur_ctx.u_box.texel_size, 1.0f / w, 1.0f / h);
  glUniform1f(blur_ctx.u_box.radius, blur_radius);
  draw_quad();

  // V: ping_tex -> pong_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, pong_fbo);
  glBindTexture(GL_TEXTURE_2D, ping_tex);
  glUseProgram(blur_ctx.prog_box_v);
  glUniform1i(blur_ctx.u_box.tex, 0);
  glUniform2f(blur_ctx.u_box.texel_size, 1.0f / w, 1.0f / h);
  glUniform1f(blur_ctx.u_box.radius, blur_radius);
  draw_quad();

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void gaussian_pass(GLuint src_tex, GLuint ping_fbo, GLuint ping_tex,
    GLuint pong_fbo, int w, int h) {
  // src_tex -> ping_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, ping_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glUseProgram(blur_ctx.prog_gauss_h);
  glUniform1i(blur_ctx.u_gauss.tex, 0);
  glUniform2f(blur_ctx.u_gauss.texel_size, 1.0f/w, 1.0f/h);
  glUniform1f(blur_ctx.u_gauss.radius, blur_radius);
  draw_quad();

  // ping_tex -> pong_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, pong_fbo);
  glBindTexture(GL_TEXTURE_2D, ping_tex);
  glUseProgram(blur_ctx.prog_gauss_v);
  glUniform1i(blur_ctx.u_gauss.tex, 0);
  glUniform2f(blur_ctx.u_gauss.texel_size, 1.0f/w, 1.0f/h);
  glUniform1f(blur_ctx.u_gauss.radius, blur_radius);
  draw_quad();

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static GLuint apply_blur(struct bwm_blur_output_ctx *ctx,
  	GLuint src_tex, int w, int h) {
  if (blur_passes <= 0 || blur_algorithm == BLUR_ALGORITHM_NONE)
    return src_tex;

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);

  int ping = 0;
  GLuint current = src_tex;

  for (int i = 0; i < blur_passes; i++) {
    int pong = ping ^ 1;
    if (blur_algorithm == BLUR_ALGORITHM_GAUSSIAN) {
      gaussian_pass(current, ctx->fbo[ping], ctx->tex[ping],
        ctx->fbo[pong], w, h);
      current = ctx->tex[pong];
      ping = pong;
    } else if (blur_algorithm == BLUR_ALGORITHM_BOX) {
      box_pass(current, ctx->fbo[ping], ctx->tex[ping],
        ctx->fbo[pong], w, h);
      current = ctx->tex[pong];
      ping = pong;
    } else {
      blur_pass(current, ctx->fbo[ping], w, h, i);
      current = ctx->tex[ping];
      ping ^= 1;
    }
  }
  return current;
}

bool blur_init(void) {
  blur_ctx = (struct bwm_blur_ctx){0};

  if (!wlr_renderer_is_gles2(server.renderer)) {
    wlr_log(WLR_INFO, "blur: renderer is not GLES2 – blur disabled");
    return false;
  }

  struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server.renderer);
  s_egl_display = wlr_egl_get_display(egl);
  s_egl_context = wlr_egl_get_context(egl);

  if (!egl_make_current()) {
    wlr_log(WLR_ERROR, "blur: failed to make EGL context current");
    return false;
  }

  blur_ctx.prog_kawase    = link_program(frag_kawase_src);
  blur_ctx.prog_gauss_h   = link_program(frag_gauss_h_src);
  blur_ctx.prog_gauss_v   = link_program(frag_gauss_v_src);
  blur_ctx.prog_box_h     = link_program(frag_box_h_src);
  blur_ctx.prog_box_v     = link_program(frag_box_v_src);
  blur_ctx.prog_blit      = link_program(frag_blit_src);
  blur_ctx.prog_mica_tint = link_program(frag_mica_tint_src);
  blur_ctx.prog_ext_blit  = link_program(frag_ext_blit_src);

  if (!blur_ctx.prog_kawase || !blur_ctx.prog_gauss_h || !blur_ctx.prog_gauss_v ||
      !blur_ctx.prog_box_h  || !blur_ctx.prog_box_v   ||
      !blur_ctx.prog_blit   || !blur_ctx.prog_mica_tint) {
    wlr_log(WLR_ERROR, "blur: one or more required shaders failed to compile");
    egl_unset_current();
    return false;
  }

  blur_ctx.u_kawase.tex = glGetUniformLocation(blur_ctx.prog_kawase, "tex");
  blur_ctx.u_kawase.halfpixel = glGetUniformLocation(blur_ctx.prog_kawase, "halfpixel");
  blur_ctx.u_kawase.offset = glGetUniformLocation(blur_ctx.prog_kawase, "offset");

  blur_ctx.u_gauss.tex = glGetUniformLocation(blur_ctx.prog_gauss_h, "tex");
  blur_ctx.u_gauss.texel_size = glGetUniformLocation(blur_ctx.prog_gauss_h, "texel_size");
  blur_ctx.u_gauss.radius = glGetUniformLocation(blur_ctx.prog_gauss_h, "radius");

  blur_ctx.u_box.tex = glGetUniformLocation(blur_ctx.prog_box_h, "tex");
  blur_ctx.u_box.texel_size = glGetUniformLocation(blur_ctx.prog_box_h, "texel_size");
  blur_ctx.u_box.radius = glGetUniformLocation(blur_ctx.prog_box_h, "radius");

  blur_ctx.u_blit.tex = glGetUniformLocation(blur_ctx.prog_blit, "tex");

  if (blur_ctx.prog_ext_blit)
    blur_ctx.u_ext_blit.tex = glGetUniformLocation(blur_ctx.prog_ext_blit, "tex");

  blur_ctx.u_mica.tex = glGetUniformLocation(blur_ctx.prog_mica_tint, "tex");
  blur_ctx.u_mica.tint = glGetUniformLocation(blur_ctx.prog_mica_tint, "tint");
  blur_ctx.u_mica.tint_strength = glGetUniformLocation(blur_ctx.prog_mica_tint, "tint_strength");

  blur_ctx.attr_pos = 0;

  static const float quad[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,
    -1.0f,  1.0f,   1.0f,  1.0f,
  };
  glGenBuffers(1, &blur_ctx.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, blur_ctx.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  egl_unset_current();
  blur_ctx.available = true;
  wlr_log(WLR_INFO, "blur: initialised (GLES2)");
  return true;
}

void blur_fini(void) {
  if (!blur_ctx.available) return;
  egl_make_current();
  glDeleteProgram(blur_ctx.prog_kawase);
  glDeleteProgram(blur_ctx.prog_gauss_h);
  glDeleteProgram(blur_ctx.prog_gauss_v);
  glDeleteProgram(blur_ctx.prog_box_h);
  glDeleteProgram(blur_ctx.prog_box_v);
  glDeleteProgram(blur_ctx.prog_blit);
  glDeleteProgram(blur_ctx.prog_mica_tint);
  if (blur_ctx.prog_ext_blit)
    glDeleteProgram(blur_ctx.prog_ext_blit);
  glDeleteBuffers(1, &blur_ctx.vbo);
  egl_unset_current();
  blur_ctx = (struct bwm_blur_ctx){0};
}

struct bwm_blur_output_ctx *blur_output_init(int width, int height) {
  if (!blur_ctx.available) return NULL;

  struct bwm_blur_output_ctx *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->width  = width;
  ctx->height = height;
  int ds = blur_downsample > 0 ? blur_downsample : 1;
  ctx->blur_w = (width  / ds) > 0 ? (width  / ds) : 1;
  ctx->blur_h = (height / ds) > 0 ? (height / ds) : 1;

  egl_make_current();
  bool ok = create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[0], &ctx->tex[0]) &&
    create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[1], &ctx->tex[1]);
  egl_unset_current();

  if (!ok) {
    free(ctx);
    return NULL;
  }

  if (!create_capture_output(ctx, width, height)) {
    wlr_log(WLR_ERROR, "blur: failed to create capture output");
    egl_make_current();
    destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
    destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
    egl_unset_current();
    free(ctx);
    return NULL;
  }

  ctx->mica_dirty = true;
  return ctx;
}

void blur_output_fini(struct bwm_blur_output_ctx *ctx) {
  if (!ctx) return;
  if (blur_ctx.available) {
    egl_make_current();
    destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
    destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
    egl_unset_current();
  }
  if (ctx->mica_buf) {
    wlr_buffer_unlock(ctx->mica_buf);
    ctx->mica_buf = NULL;
    ctx->mica_buf_fbo = 0;
  }
  destroy_capture_output(ctx);
  free(ctx);
}

void blur_output_resize(struct bwm_blur_output_ctx *ctx, int width, int height) {
  if (!ctx || !blur_ctx.available) return;
  int ds = blur_downsample > 0 ? blur_downsample : 1;
  int new_bw = (width  / ds) > 0 ? (width  / ds) : 1;
  int new_bh = (height / ds) > 0 ? (height / ds) : 1;
  if (ctx->width == width && ctx->height == height &&
      ctx->blur_w == new_bw && ctx->blur_h == new_bh) return;

  egl_make_current();
  destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
  destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
  ctx->width  = width;
  ctx->height = height;
  ctx->blur_w = new_bw;
  ctx->blur_h = new_bh;
  create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[0], &ctx->tex[0]);
  create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[1], &ctx->tex[1]);
  egl_unset_current();

  if (ctx->mica_buf) {
    wlr_buffer_unlock(ctx->mica_buf);
    ctx->mica_buf = NULL;
    ctx->mica_buf_fbo = 0;
  }

  /* Free per-toplevel blur buffers since output dimensions changed */
  struct bwm_toplevel *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (tl->blur_buf) {
      wlr_buffer_unlock(tl->blur_buf);
      tl->blur_buf = NULL;
      tl->blur_buf_fbo = 0;
    }
  }

  ctx->mica_dirty = true;
}

void blur_invalidate_mica(struct bwm_blur_output_ctx *ctx) {
  if (ctx) ctx->mica_dirty = true;
}

static bool compute_src_box(struct bwm_output *output, const struct wlr_box *r,
    struct wlr_fbox *src_out, int *dw_out, int *dh_out) {
  float bw = (float)output->width;
  float bh = (float)output->height;
  float sx = (float)(r->x - output->lx);
  float sy = (float)(r->y - output->ly);
  float sw = (float)r->width;
  float sh = (float)r->height;

  if (sx < 0.0f) {
	  sw += sx;
	  sx = 0.0f;
  }
  if (sy < 0.0f) {
  	sh += sy;
   	sy = 0.0f;
  }
  if (sx >= bw || sy >= bh || sw <= 0.0f || sh <= 0.0f)
    return false;

  if (sx + sw > bw) sw = bw - sx;
  if (sy + sh > bh) sh = bh - sy;
  if (sw <= 0.0f || sh <= 0.0f)
    return false;

  *src_out = (struct wlr_fbox){
    .x = sx,
    .y = sy,
    .width = sw,
    .height = sh
  };
  *dw_out = (int)sw;
  *dh_out = (int)sh;
  return true;
}
static GLuint capture_bg_to_tex1(struct bwm_output *output, struct bwm_blur_output_ctx *ctx,
    struct wlr_scene_output *real_scene_output, bool mica_only,
    struct bwm_toplevel *target_tl) {
  int w = output->width, h = output->height;

  if (!ctx->capture_output || !ctx->capture_scene_output)
    return 0;

  wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);

  if (w <= 0 || h <= 0)
    return 0;

  wlr_scene_node_set_enabled(&server.top_tree->node, false);
  wlr_scene_node_set_enabled(&server.full_tree->node, false);
  wlr_scene_node_set_enabled(&server.over_tree->node, false);
  wlr_scene_node_set_enabled(&server.lock_tree->node, false);

  if (mica_only) {
    wlr_scene_node_set_enabled(&server.tile_tree->node, false);
    wlr_scene_node_set_enabled(&server.float_tree->node, false);
  }

  struct bwm_toplevel *tl;
  if (target_tl) {
	  // per-window
    target_tl->blur_scene_hidden = false;
    if (target_tl->scene_tree && target_tl->scene_tree->node.enabled) {
      wlr_scene_node_set_enabled(&target_tl->scene_tree->node, false);
      target_tl->blur_scene_hidden = true;
    }
  } else {
   	// hide everything with mica
    wl_list_for_each(tl, &server.toplevels, link) {
      tl->blur_scene_hidden = false;
      if ((tl->blur_node || tl->mica_node) && tl->scene_tree &&
          tl->scene_tree->node.enabled) {
        wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
        tl->blur_scene_hidden = true;
      }
    }
  }

  wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

  struct wlr_output_state cap_state;
  wlr_output_state_init(&cap_state);
  wlr_output_state_set_enabled(&cap_state, true);
  wlr_output_state_set_custom_mode(&cap_state, w, h, 0);

  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output,
    &cap_state, NULL);

  if (ok)
    wlr_output_commit_state(ctx->capture_output, &cap_state);

  egl_make_current();
  glFlush();

  if (target_tl) {
    if (target_tl->blur_scene_hidden)
      wlr_scene_node_set_enabled(&target_tl->scene_tree->node, true);
  } else {
    wl_list_for_each(tl, &server.toplevels, link)
      if (tl->blur_scene_hidden)
        wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
  }

  wlr_scene_node_set_enabled(&server.top_tree->node, true);
  wlr_scene_node_set_enabled(&server.full_tree->node, true);
  wlr_scene_node_set_enabled(&server.over_tree->node, true);
  wlr_scene_node_set_enabled(&server.lock_tree->node, true);
  if (mica_only) {
    wlr_scene_node_set_enabled(&server.tile_tree->node, true);
    wlr_scene_node_set_enabled(&server.float_tree->node, true);
  }

  // prevent output overlap by parking offscreen
  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

  wlr_damage_ring_add_whole(&real_scene_output->damage_ring);

  if (!ok || !cap_state.buffer) {
    egl_unset_current();
    wlr_output_state_finish(&cap_state);
    return 0;
  }

  GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, cap_state.buffer);
  GLuint result = 0;

  if (capture_fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
    GLint attach_type = 0, attach_name = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
    	GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attach_type);
    if (attach_type == GL_TEXTURE) {
      glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      	GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attach_name);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (attach_type == GL_TEXTURE && attach_name > 0 && blur_ctx.prog_ext_blit) {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
      glViewport(0, 0, ctx->blur_w, ctx->blur_h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)attach_name);
      glUseProgram(blur_ctx.prog_ext_blit);
      glUniform1i(blur_ctx.u_ext_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->tex[1];
    } else if (attach_type == GL_TEXTURE && attach_name > 0) {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
      glViewport(0, 0, ctx->blur_w, ctx->blur_h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, (GLuint)attach_name);
      glUseProgram(blur_ctx.prog_blit);
      glUniform1i(blur_ctx.u_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->tex[1];
    } else if (attach_type == GL_RENDERBUFFER) {
      unsigned char *pixels = malloc((size_t)w * h * 4);
      if (pixels) {
        glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint tmp_tex;
        glGenTextures(1, &tmp_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tmp_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        free(pixels);

        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
        glViewport(0, 0, ctx->blur_w, ctx->blur_h);
        glUseProgram(blur_ctx.prog_blit);
        glUniform1i(blur_ctx.u_blit.tex, 0);
        draw_quad();
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &tmp_tex);
        result = ctx->tex[1];
      }
    }
  }

  egl_unset_current();
  wlr_output_state_finish(&cap_state);

  return result;
}

// ensure buf/fbo are allocated; returns fbo or 0 on failure. Must be called
// inside egl_make_current(). Allocates once; subsequent calls reuse the buffer.
static GLuint ensure_output_buf(struct wlr_buffer **buf_out, GLuint *fbo_out,
    int w, int h) {
  if (*buf_out)
    return *fbo_out;

  const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(server.renderer);
  const struct wlr_drm_format *fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
  if (!fmt) fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;
  if (!fmt) return 0;

  struct wlr_buffer *buf = wlr_allocator_create_buffer(server.allocator, w, h, fmt);
  if (!buf) return 0;

  GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, buf);
  if (!fbo) {
    wlr_buffer_drop(buf);
    return 0;
  }

  wlr_buffer_lock(buf);
  wlr_buffer_drop(buf);
  *buf_out = buf;
  *fbo_out = fbo;
  return fbo;
}

static bool rebuild_live_blur(struct bwm_output *output,
		struct wlr_scene_output *scene_output) {
  struct bwm_blur_output_ctx *ctx = output->blur_ctx;
  int w = output->width, h = output->height;
  bool any = false;

  struct bwm_toplevel *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->monitor || tl->node->monitor->output != output) continue;

    GLuint src = capture_bg_to_tex1(output, ctx, scene_output, false, tl);
    if (!src) continue;

    egl_make_current();
    GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

    GLuint dest_fbo = ensure_output_buf(&tl->blur_buf, &tl->blur_buf_fbo, w, h);
    if (!dest_fbo) {
      egl_unset_current();
      continue;
    }

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
    glViewport(0, 0, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred);
    glUseProgram(blur_ctx.prog_blit);
    glUniform1i(blur_ctx.u_blit.tex, 0);
    draw_quad();
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    egl_unset_current();
    any = true;
  }
  return any;
}

static void push_blur_to_toplevels(struct bwm_output *output) {
  struct bwm_toplevel *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur_node || !tl->node) continue;
    monitor_t *m = tl->node->monitor;
    if (!m || m->output != output) continue;

    if (!tl->blur_buf) {
      wlr_scene_buffer_set_buffer(tl->blur_node, NULL);
      continue;
    }

    wlr_scene_buffer_set_buffer(tl->blur_node, tl->blur_buf);

    client_t *c = tl->node->client;
    struct wlr_box r;
    if (c->state == STATE_FULLSCREEN && tl->node->monitor)
      r = tl->node->monitor->rectangle;
    else if (c->state == STATE_FLOATING)
      r = c->floating_rectangle;
    else
      r = c->tiled_rectangle;

    struct wlr_fbox src; int dw, dh;
    if (!compute_src_box(output, &r, &src, &dw, &dh)) {
      wlr_scene_buffer_set_buffer(tl->blur_node, NULL);
      wlr_scene_node_set_position(&tl->blur_node->node, 0, 0);
      continue;
    }
    int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
    int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
    wlr_scene_node_set_position(&tl->blur_node->node, node_ox, node_oy);
    wlr_scene_buffer_set_source_box(tl->blur_node, &src);
    wlr_scene_buffer_set_dest_size(tl->blur_node, dw, dh);
  }
}

static bool rebuild_mica(struct bwm_output *output, struct wlr_scene_output *scene_output) {
  struct bwm_blur_output_ctx *ctx = output->blur_ctx;
  int w = output->width, h = output->height;

  GLuint src = capture_bg_to_tex1(output, ctx, scene_output, true, NULL);
  if (!src) return false;

  egl_make_current();
  GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

  GLuint dest_fbo = ensure_output_buf(&ctx->mica_buf, &ctx->mica_buf_fbo, w, h);
  if (!dest_fbo) {
    egl_unset_current();
    return false;
  }

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, blurred);
  glUseProgram(blur_ctx.prog_mica_tint);
  glUniform1i(blur_ctx.u_mica.tex, 0);
  glUniform4fv(blur_ctx.u_mica.tint, 1, mica_tint);
  glUniform1f(blur_ctx.u_mica.tint_strength, mica_tint_strength);
  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glFlush();
  egl_unset_current();

  ctx->mica_dirty = false;
  return true;
}

static void push_mica_to_toplevels(struct bwm_output *output) {
  struct wlr_buffer *buf = output->blur_ctx->mica_buf;
  if (!buf) return;

  struct bwm_toplevel *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->mica_node || !tl->node) continue;
    monitor_t *m = tl->node->monitor;
    if (!m || m->output != output) continue;

    wlr_scene_buffer_set_buffer(tl->mica_node, buf);

    client_t *c = tl->node->client;
    struct wlr_box r;
    if (c->state == STATE_FULLSCREEN && tl->node->monitor)
      r = tl->node->monitor->rectangle;
    else if (c->state == STATE_FLOATING)
      r = c->floating_rectangle;
    else
      r = c->tiled_rectangle;

    struct wlr_fbox src;
    int dw, dh;
    if (!compute_src_box(output, &r, &src, &dw, &dh)) {
      wlr_scene_buffer_set_buffer(tl->mica_node, NULL);
      wlr_scene_node_set_position(&tl->mica_node->node, 0, 0);
      continue;
    }
    int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
    int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
    wlr_scene_node_set_position(&tl->mica_node->node, node_ox, node_oy);
    wlr_scene_buffer_set_source_box(tl->mica_node, &src);
    wlr_scene_buffer_set_dest_size(tl->mica_node, dw, dh);
  }
}

void blur_output_frame(struct bwm_output *output, struct wlr_scene_output *scene_output) {
  if (!blur_ctx.available) return;
  struct bwm_blur_output_ctx *ctx = output->blur_ctx;
  if (!ctx) return;

  if (ctx->width != output->width || ctx->height != output->height)
    blur_output_resize(ctx, output->width, output->height);

  if (blur_enabled) {
    bool any_blur = false;
    struct bwm_toplevel *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (tl->blur_node && tl->node && tl->node->client && tl->node->client->shown &&
          tl->node->monitor && tl->node->monitor->output == output) {
        any_blur = true;
        break;
      }
    }
    if (any_blur) {
      rebuild_live_blur(output, scene_output);
      push_blur_to_toplevels(output);
    }
  }

  if (mica_enabled && ctx->mica_dirty)
    rebuild_mica(output, scene_output);

  if (mica_enabled && ctx->mica_buf)
    push_mica_to_toplevels(output);
}

enum blur_algorithm blur_algorithm_from_str(const char *str) {
  if (!str)                         return BLUR_ALGORITHM_KAWASE;
  if (strcmp(str, "kawase")   == 0) return BLUR_ALGORITHM_KAWASE;
  if (strcmp(str, "gaussian") == 0) return BLUR_ALGORITHM_GAUSSIAN;
  if (strcmp(str, "box")      == 0) return BLUR_ALGORITHM_BOX;
  if (strcmp(str, "none")     == 0) return BLUR_ALGORITHM_NONE;
  wlr_log(WLR_ERROR, "blur: unknown algorithm '%s', using kawase", str);
  return BLUR_ALGORITHM_KAWASE;
}

const char *blur_algorithm_to_str(enum blur_algorithm algo) {
  switch (algo) {
  case BLUR_ALGORITHM_KAWASE:   return "kawase";
  case BLUR_ALGORITHM_GAUSSIAN: return "gaussian";
  case BLUR_ALGORITHM_BOX:      return "box";
  default:                      return "none";
  }
}
