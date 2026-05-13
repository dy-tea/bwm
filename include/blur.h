#pragma once

#include <stdbool.h>
#include <GLES2/gl2.h>
#include <wlr/util/box.h>

struct bwm_output;
struct wlr_scene_output;
struct wlr_buffer;
struct wlr_backend;
struct wlr_output;
struct wlr_scene_buffer;

enum blur_algorithm {
  BLUR_ALGORITHM_NONE,
  BLUR_ALGORITHM_KAWASE,
  BLUR_ALGORITHM_GAUSSIAN,
  BLUR_ALGORITHM_BOX,
  BLUR_ALGORITHM_REFRACTION,
  BLUR_ALGORITHM_LENS_REFRACTION,
};

extern bool blur_enabled;
extern enum blur_algorithm blur_algorithm;
extern int blur_passes;
extern float blur_radius;
extern int blur_downsample;

extern float blur_vibrancy;
extern float blur_vibrancy_darkness;
extern float blur_noise_strength;
extern float blur_brightness;
extern float blur_contrast;

extern bool mica_enabled;
extern float mica_tint[4];
extern float mica_tint_strength;
extern float acrylic_tint[4];
extern float acrylic_tint_strength;
extern float acrylic_noise_strength;
extern float acrylic_light_anchor[2];
extern int acrylic_blur_passes;

extern bool screen_shader_enabled;

extern float refraction_strength;
extern float refraction_edge_size_px;
extern float refraction_corner_radius_px;
extern float refraction_normal_pow;
extern float refraction_rgb_fringing;
extern int refraction_texture_repeat_mode;
extern float refraction_offset;

struct bwm_blur_output_ctx {
  int width, height;
  int blur_w, blur_h;

  GLuint fbo[2];
  GLuint tex[2];

  GLuint screen_fbo;  /* full-res intermediate for screen shader */
  GLuint screen_tex;

  struct wlr_buffer *mica_buf;
  GLuint mica_buf_fbo;
  bool mica_dirty;

  struct wlr_buffer *screen_shader_buf;
  GLuint screen_shader_buf_fbo;
  struct wlr_scene_buffer *screen_shader_node;

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
  GLuint prog_acrylic_tint;
  GLuint prog_refraction;
  GLuint prog_ext_blit;
  GLuint prog_border;
  GLuint prog_corner_mask;

  struct {
    GLint tex, halfpixel, offset, noise_strength, vibrancy, vibrancy_darkness, brightness, contrast;
  } u_kawase;
  struct {
    GLint tex, texel_size, radius, vibrancy, vibrancy_darkness, brightness, contrast;
  } u_gauss;
  struct {
    GLint tex, texel_size, radius, vibrancy, vibrancy_darkness, brightness, contrast;
  } u_box;
  struct {
    GLint tex;
  } u_blit;
  struct {
    GLint tex, tint, tint_strength;
  } u_mica;
  struct {
    GLint tex, tint, tint_strength, noise_strength, resolution, light_anchor;
  } u_acrylic;
  struct {
    GLint tex;
    GLint offset;
    GLint halfpixel;
    GLint refraction_rect_size;
    GLint refraction_edge_size_pixels;
    GLint refraction_corner_radius_pixels;
    GLint refraction_strength;
    GLint refraction_normal_pow;
    GLint refraction_RGB_fringing;
    GLint refraction_texture_repeat_mode;
    GLint refraction_mode;
  } u_refraction;
  struct {
    GLint tex;
  } u_ext_blit;
  struct {
    GLint resolution, border_radius, border_width_px, border_color;
  } u_border;
  struct {
    GLint tex, win_pos_uv, win_size_uv, win_size_px, border_radius_px;
  } u_corner_mask;

  GLuint vbo;
  GLint attr_pos;
};

extern struct bwm_blur_ctx blur_ctx;

bool blur_init(void);
void blur_fini(void);

struct bwm_blur_output_ctx *blur_output_init(int width, int height);
void blur_output_fini(struct bwm_blur_output_ctx *ctx);
void blur_output_resize(struct bwm_blur_output_ctx *ctx, int width, int height, struct bwm_output *output);

void blur_invalidate_mica(struct bwm_blur_output_ctx *ctx);

void blur_output_frame(struct bwm_output *output, struct wlr_scene_output *scene_output);

enum blur_algorithm blur_algorithm_from_str(const char *str);
const char *blur_algorithm_to_str(enum blur_algorithm algo);

bool screen_shader_set(const char *name);
bool screen_shader_load_file(const char *path);
void screen_shader_clear(void);
const char *screen_shader_get_name(void);
