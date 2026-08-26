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

#define BLUR_MAX_LEVELS 10

enum be_resource_state {
	BE_RESOURCE_UNINITIALIZED,
	BE_RESOURCE_SHADER_READ,
	BE_RESOURCE_COLOR_ATTACHMENT,
	BE_RESOURCE_TRANSFER_SRC,
	BE_RESOURCE_TRANSFER_DST,
};

typedef struct {
	uint64_t native_handle[2];
	int width, height;
	enum be_resource_state state;
	bool owned;
	uint32_t generation;
} be_buffer_t;

typedef struct {
	uint64_t handle;
	int width, height;
	enum be_resource_state state;
	uint32_t generation;
	bool valid;
} be_effect_resource_t;

typedef struct {
	be_buffer_t capture;
	be_buffer_t ping, pong;
	be_buffer_t blur_scratch;
	be_buffer_t staging;
	be_buffer_t screen_shader;
	be_buffer_t blur_levels[BLUR_MAX_LEVELS];
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
	float vibrancy, vibrancy_darkness;
	float noise_strength;
	float brightness;
	float contrast;
	bool full_res;
	float offset;
	float saturation;
	float refraction_strength;
	float refraction_edge_size_px;
	float refraction_corner_radius_px;
	float refraction_normal_pow;
	float refraction_rgb_fringing;
	int refraction_texture_repeat_mode;
	float refraction_offset;
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
	float bg_u, bg_v;
	float bg_sw, bg_sh;
	bool pre_blit;
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

	bool (*blit)(be_effect_resource_t src, be_effect_resource_t dst, int w, int h,
		const pixman_box32_t *scissor, int n_scissor);

	bool (*blur)(be_output_state_t *state, be_effect_resource_t src, int src_w, int src_h,
		struct be_blur_params *p, be_effect_resource_t dst, const pixman_box32_t *scissor, int n_scissor,
		be_effect_resource_t *out_resource);

	bool (*apply_mica_tint)(be_output_state_t *state, be_effect_resource_t bg, float tint[4],
		float tint_strength, be_effect_resource_t dst);

	bool (*apply_acrylic)(be_output_state_t *state, be_effect_resource_t bg,
		struct be_acrylic_params *p, be_effect_resource_t dst);

	bool (*render_shadow)(struct be_shadow_params *p, be_effect_resource_t dst);

	bool (*render_border)(struct be_border_params *p, be_effect_resource_t dst);

	bool (*apply_corner_mask)(be_output_state_t *state, be_effect_resource_t dst,
		be_effect_resource_t bg, struct be_corner_mask_params *p);

	bool (*apply_screen_shader)(be_effect_resource_t src, be_effect_resource_t dst, int w, int h,
		struct be_screen_shader_params *p);

	bool (*capture_readback)(struct wlr_buffer *capture_buffer, be_output_state_t *state,
		be_effect_resource_t dst, int dst_x, int dst_y, int dst_w, int dst_h, int src_x, int src_y,
		int src_w, int src_h, uint32_t generation, be_effect_resource_t *out_resource);

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

/*
 * Canonical resource construction helpers.
 * These are the only approved way to create be_effect_resource_t values
 * from backend-owned buffers. Never construct effect resources manually
 * from raw native handles.
 */

static inline be_effect_resource_t be_buffer_resource(const be_buffer_t *buffer,
		enum be_resource_state expected_state, uint32_t generation) {
	if (!buffer || buffer->native_handle[1] == 0 || buffer->width <= 0 || buffer->height <= 0) {
		return (be_effect_resource_t){0};
	}
	return (be_effect_resource_t){
		.handle = buffer->native_handle[1],
		.width = buffer->width,
		.height = buffer->height,
		.state = buffer->state,
		.generation = generation,
		.valid = buffer->state == expected_state,
	};
}

static inline be_effect_resource_t be_buffer_target(be_buffer_t *buffer, int width, int height,
		uint32_t generation) {
	if (!buffer || buffer->native_handle[0] == 0 || width <= 0 || height <= 0) {
		return (be_effect_resource_t){0};
	}
	return (be_effect_resource_t){
		.handle = buffer->native_handle[0],
		.width = width,
		.height = height,
		.state = BE_RESOURCE_COLOR_ATTACHMENT,
		.generation = generation,
		.valid = true,
	};
}

static inline be_effect_resource_t be_buffer_target_from_buffer(const be_buffer_t *buffer,
		uint32_t generation) {
	if (!buffer || buffer->native_handle[0] == 0) {
		return (be_effect_resource_t){0};
	}
	return (be_effect_resource_t){
		.handle = buffer->native_handle[0],
		.width = buffer->width,
		.height = buffer->height,
		.state = BE_RESOURCE_COLOR_ATTACHMENT,
		.generation = generation,
		.valid = buffer->width > 0 && buffer->height > 0,
	};
}

static inline be_effect_resource_t be_buffer_resource_from_buffer(const be_buffer_t *buffer,
		enum be_resource_state expected_state, uint32_t generation) {
	if (!buffer || buffer->native_handle[1] == 0) {
		return (be_effect_resource_t){0};
	}
	return (be_effect_resource_t){
		.handle = buffer->native_handle[1],
		.width = buffer->width,
		.height = buffer->height,
		.state = buffer->state,
		.generation = generation,
		.valid = buffer->width > 0 && buffer->height > 0 && buffer->state == expected_state,
	};
}

/*
 * Resource validation helpers.
 */

static inline bool be_resource_valid(const be_effect_resource_t *resource) {
	return resource && resource->valid && resource->handle != 0 && resource->width > 0 &&
		resource->height > 0;
}

static inline bool be_resource_matches(const be_effect_resource_t *resource, int w, int h) {
	return resource && resource->width == w && resource->height == h;
}

static inline bool be_resources_alias(const be_effect_resource_t *a,
		const be_effect_resource_t *b) {
	if (!a || !b)
		return false;
	return a->handle == b->handle;
}

static inline bool be_resource_readable(const be_effect_resource_t *resource) {
	return be_resource_valid(resource) && resource->state == BE_RESOURCE_SHADER_READ;
}
