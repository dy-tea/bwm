#pragma once

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_buffer.h>
struct wlr_renderer;
struct wlr_allocator;
struct wlr_scene;
struct wlr_scene_output;
struct wlr_output_state;

typedef struct {
	struct wlr_buffer *buf;
	uint64_t native_handle[2];
	int width, height;
} be_buffer_t;

typedef struct {
	be_buffer_t ping, pong;
	be_buffer_t staging;
	be_buffer_t screen_shader;
} be_output_state_t;

enum blur_algorithm {
	BLUR_ALGORITHM_NONE,
	BLUR_ALGORITHM_KAWASE,
	BLUR_ALGORITHM_GAUSSIAN,
	BLUR_ALGORITHM_BOX,
	BLUR_ALGORITHM_REFRACTION,
	BLUR_ALGORITHM_LENS_REFRACTION,
};

struct be_blur_params {
	enum blur_algorithm algorithm;
	int passes;
	float radius;
	int downsample;
	float vibrancy, vibrancy_darkness;
	float noise_strength;
	float brightness;
	float contrast;
	float refraction_strength;
	float refraction_edge_size_px;
	float refraction_corner_radius_px;
	float refraction_normal_pow;
	float refraction_rgb_fringing;
	int refraction_texture_repeat_mode;
	float refraction_offset;
	float refraction_noise_strength;
	float refraction_noise_scale;
};

struct be_shadow_params {
	float shadow_size;
	float shadow_offset_x, shadow_offset_y;
	float shadow_color[4];
	float border_radius;
	float inner_width, inner_height;
	float hole_x, hole_y, hole_width, hole_height;
	float scale;
	int buf_w, buf_h;
};

struct be_border_params {
	float res_w, res_h;
	float border_radius;
	float border_width_px;
	float border_color[4];
	float scale;
	float gradient_colors[40];
	int gradient_count;
	float gradient_angle;
	float gradient2_colors[40];
	int gradient2_count;
	float gradient2_angle;
	float gradient_lerp;
	int buf_w, buf_h;
};

struct be_corner_mask_params {
	int out_w, out_h;
	float win_u, win_v;
	float win_sw, win_sh;
	float win_size_px_w, win_size_px_h;
	float border_radius_px;
	float scale;
	bool pre_blit; // true if dst already has content
};

struct be_acrylic_params {
	float tint[4];
	float tint_strength;
	float noise_strength;
	float res_w, res_h;
	float light_anchor_x, light_anchor_y;
	int blur_passes;
	float blur_radius;
};

enum screen_shader_type {
	SCREEN_SHADER_NONE,
	SCREEN_SHADER_GRAYSCALE,
	SCREEN_SHADER_INVERT,
	SCREEN_SHADER_SEPIA,
	SCREEN_SHADER_NIGHTLIGHT,
	SCREEN_SHADER_CUSTOM,
};

struct be_screen_shader_params {
	enum screen_shader_type type;
	const char *custom_glsl;
	float time;
	float scale;
};

typedef struct effects_backend_t {
	bool (*init)(struct wlr_renderer *r, struct wlr_allocator *a);
	void (*fini)(void);

	bool (*output_init)(be_output_state_t *state, int width, int height, int blur_w, int blur_h);
	void (*output_fini)(be_output_state_t *state);
	void (*output_resize)(be_output_state_t *state, int width, int height, int blur_w, int blur_h);

	bool (*ensure_buffer)(struct wlr_buffer **buf, uint64_t native[2], int w, int h,
		struct wlr_renderer *r, struct wlr_allocator *a);
	void (*destroy_buffer)(struct wlr_buffer *buf, uint64_t native[2]);

	void (*frame_begin)(void);
	void (*frame_end)(void);

	bool (*blit)(uint64_t src_tex, uint64_t dst_fbo, int w, int h, const pixman_box32_t *scissor,
		int n_scissor);

	bool (*blur)(be_output_state_t *state, uint64_t src_handle, int src_w, int src_h,
		struct be_blur_params *p, uint64_t *out_handle);

	bool (*apply_mica_tint)(be_output_state_t *state, uint64_t bg_handle, float tint[4],
		float tint_strength, uint64_t dst_fbo, int w, int h);

	bool (*apply_acrylic)(be_output_state_t *state, uint64_t bg_handle, struct be_acrylic_params *p,
		uint64_t dst_fbo, int w, int h);

	bool (*render_shadow)(struct be_shadow_params *p, uint64_t dst_fbo);

	bool (*render_border)(struct be_border_params *p, uint64_t dst_fbo);

	bool (*apply_corner_mask)(be_output_state_t *state, uint64_t dst_fbo, int dst_w, int dst_h,
		uint64_t bg_tex, struct be_corner_mask_params *p);

	bool (*apply_screen_shader)(uint64_t src_tex, uint64_t dst_fbo, int w, int h,
		struct be_screen_shader_params *p);

	bool (*capture_readback)(struct wlr_buffer *capture_buffer, be_output_state_t *state,
		uint64_t dst_fbo, int dst_w, int dst_h, int src_w, int src_h, uint64_t *out_tex);

	const char *(*get_screen_shader_source)(const char *name);
	bool (*compile_screen_shader)(const char *frag_src);
	void (*destroy_screen_shader)(void);
} effects_backend_t;

extern const effects_backend_t *effects_backend;

static inline void effects_destroy_buffer(struct wlr_buffer **buf, uint64_t *native) {
	if (!*buf)
		return;
	if (effects_backend && native)
		effects_backend->destroy_buffer(*buf, native);
	else
		wlr_buffer_unlock(*buf);
	*buf = NULL;
}
