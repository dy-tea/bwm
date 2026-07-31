#pragma once

#include "effects_backend.h"

#include <pixman.h>
#include <stdbool.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

typedef struct output_t output_t;
struct wlr_scene_output;
struct wlr_buffer;
struct wlr_backend;
struct wlr_output;
struct wlr_scene_buffer;

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

typedef struct effects_output_t {
	int width, height;
	int blur_w, blur_h;

	be_output_state_t be_state;

	struct wlr_buffer *mica_buf;
	uint64_t mica_native[2];
	bool mica_dirty;

	// shared blurred background, reused by every window blur
	struct wlr_buffer *blur_buf;
	uint64_t blur_native[2];

	// shared blurred layer background, reused by every layer surface blur
	struct wlr_buffer *layer_blur_buf;
	uint64_t layer_blur_native[2];

	struct wlr_buffer *screen_shader_buf;
	uint64_t screen_shader_native[2];
	struct wlr_scene_buffer *screen_shader_node;

	pixman_region32_t scratch_region_a, scratch_region_b;

	struct wlr_backend *capture_backend;
	struct wlr_output *capture_output;
	struct wlr_scene_output *capture_scene_output;
	struct wlr_output_state capture_state;

	uint64_t cached_bg_tex;
	bool bg_cache_valid;
} effects_output_t;

typedef struct effects_state_t {
	bool available;
	const effects_backend_t *backend;
	void *backend_data;
	int eviction_counter;
} effects_state_t;

extern effects_state_t effects_state;

bool effects_init(void);
void effects_fini(void);

effects_output_t *effects_output_init(int width, int height);
void effects_output_fini(effects_output_t *ctx);
void effects_output_resize(effects_output_t *ctx, int width, int height, output_t *output);

void effects_invalidate_mica(effects_output_t *ctx);

void effects_output_frame(output_t *output, struct wlr_scene_output *scene_output);
void effects_evict_buffers(void);

enum blur_algorithm blur_algorithm_from_str(const char *str);
const char *effects_algorithm_to_str(enum blur_algorithm algo);

bool screen_shader_set(const char *name);
bool screen_shader_load_file(const char *path);
void screen_shader_clear(void);
const char *screen_shader_get_name(void);
