#pragma once

#include <stdbool.h>
#include <GLES2/gl2.h>
#include <wlr/util/box.h>

struct bwm_output;
struct wlr_scene_output;
struct wlr_buffer;
struct wlr_backend;
struct wlr_output;

enum blur_algorithm {
  BLUR_ALGORITHM_NONE,
  BLUR_ALGORITHM_KAWASE,
  BLUR_ALGORITHM_GAUSSIAN,
  BLUR_ALGORITHM_BOX,
};

extern bool blur_enabled;
extern enum blur_algorithm blur_algorithm;
extern int blur_passes;
extern float blur_radius;
extern int blur_downsample;
extern bool mica_enabled;
extern float mica_tint[4];
extern float mica_tint_strength;

struct bwm_blur_output_ctx {
  int width, height;
  int blur_w, blur_h;

  GLuint fbo[2];
  GLuint tex[2];

  struct wlr_buffer *mica_buf;
  GLuint mica_buf_fbo;
  bool mica_dirty;

  struct wlr_backend *capture_backend;
  struct wlr_output *capture_output;
  struct wlr_scene_output *capture_scene_output;
};

struct bwm_blur_ctx {
  bool available;

  GLuint prog_kawase;
  GLuint prog_gauss_h;
  GLuint prog_gauss_v;
  GLuint prog_box_h;
  GLuint prog_box_v;
  GLuint prog_blit;
  GLuint prog_mica_tint;
  GLuint prog_ext_blit;

  struct {
  	GLint tex, halfpixel, offset;
  } u_kawase;
  struct {
    GLint tex, texel_size, radius;
  } u_gauss;
  struct {
    GLint tex, texel_size, radius;
  } u_box;
  struct {
    GLint tex;
  } u_blit;
  struct {
    GLint tex, tint, tint_strength;
  } u_mica;
  struct {
    GLint tex;
  } u_ext_blit;

  GLuint vbo;
  GLint attr_pos;
};

extern struct bwm_blur_ctx blur_ctx;

bool blur_init(void);
void blur_fini(void);

struct bwm_blur_output_ctx *blur_output_init(int width, int height);
void blur_output_fini(struct bwm_blur_output_ctx *ctx);
void blur_output_resize(struct bwm_blur_output_ctx *ctx, int width, int height);

void blur_invalidate_mica(struct bwm_blur_output_ctx *ctx);

void blur_output_frame(struct bwm_output *output, struct wlr_scene_output *scene_output);

enum blur_algorithm blur_algorithm_from_str(const char *str);
const char *blur_algorithm_to_str(enum blur_algorithm algo);
