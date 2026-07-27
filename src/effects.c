#include "animation.h"
#include "effects.h"
#include "effects_backend.h"
#include "layer.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"
#include "tree.h"
#include <drm_fourcc.h>
#include <pixman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend/interface.h>
#include <wlr/config.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

// private wlroots functions
extern bool wlr_renderer_is_pixman(const struct wlr_renderer *wlr_renderer);

#ifdef WLR_HAS_GLES2_RENDERER
extern bool wlr_renderer_is_gles2(const struct wlr_renderer *wlr_renderer);
extern const effects_backend_t gles2_backend;
#endif

#ifdef WLR_HAS_VULKAN_RENDERER
extern bool wlr_renderer_is_vk(const struct wlr_renderer *wlr_renderer);
extern const effects_backend_t vk_backend;
#endif

enum blur_algorithm blur_algorithm = BLUR_ALGORITHM_KAWASE;

static const struct wlr_drm_format *s_render_fmt = NULL;
bool blur_enabled = true;
int blur_passes = 1;
float blur_radius = 5.0f;
int blur_downsample = 4;

float blur_vibrancy = 0.0f;
float blur_vibrancy_darkness = 0.5f;
float blur_noise_strength = 0.0f;
float blur_brightness = 1.0f;
float blur_contrast = 1.0f;

bool mica_enabled = false;
float mica_tint[4] = {
	0.12f,
	0.12f,
	0.14f,
	1.0f
};

float mica_tint_strength = 0.35f;

float acrylic_tint[4] = {
	1.0f,
	1.0f,
	1.0f,
	1.0f
};

float acrylic_tint_strength = 0.3f;
float acrylic_noise_strength = 0.02f;
float acrylic_light_anchor[2] = {
	0.5f,
	0.5f
};

int acrylic_blur_passes = 4;

bool screen_shader_enabled = false;

float refraction_strength = 30.0f;
float refraction_edge_size_px = 18.0f;
float refraction_corner_radius_px = 8.0f;
float refraction_normal_pow = 6.0f;
float refraction_rgb_fringing = 22.0f / 30.0f;
int refraction_texture_repeat_mode = 1;
float refraction_offset = 1.0f;
float refraction_noise_strength = 0.03f;
float refraction_noise_scale = 1.0f;

float shadow_size = 8.0f;
float shadow_offset_x = 0.0f;
float shadow_offset_y = 4.0f;
float shadow_color[4] = {
	0.0f,
	0.0f,
	0.0f,
	0.5f
};

effects_state_t effects_state = {0};
const effects_backend_t *effects_backend = NULL;

static char screen_shader_name_str[256] = "none";

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

static bool create_capture_output(effects_output_t *ctx, int width, int height) {
	ctx->capture_backend = calloc(1, sizeof(struct wlr_backend));
	if (!ctx->capture_backend)
		return false;
	wlr_backend_init(ctx->capture_backend, &capture_backend_impl);
	ctx->capture_backend->buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

	ctx->capture_output = calloc(1, sizeof(struct wlr_output));
	if (!ctx->capture_output) {
		wlr_backend_finish(ctx->capture_backend);
		free(ctx->capture_backend);
		ctx->capture_backend = NULL;
		return false;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	wlr_output_init(ctx->capture_output, ctx->capture_backend, &capture_output_impl, loop, NULL);

	char name[64];
	snprintf(name, sizeof(name), "BLUR-%zu", ++capture_output_num);
	wlr_output_set_name(ctx->capture_output, name);

	wlr_output_init_render(ctx->capture_output, server.allocator, server.renderer);

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

	wlr_output_state_init(&ctx->capture_state);
	wlr_output_state_set_enabled(&ctx->capture_state, true);
	wlr_output_state_set_custom_mode(&ctx->capture_state, width, height, 0);

	wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);
	wlr_log(WLR_INFO, "blur: created capture output %s", name);
	return true;
}

static void destroy_capture_output(effects_output_t *ctx) {
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
	wlr_output_state_finish(&ctx->capture_state);
}

static bool ensure_output_buf(struct wlr_buffer **buf_out, uint64_t native[2], int w, int h) {
	return effects_backend->ensure_buffer(buf_out, native, w, h, server.renderer, server.allocator);
}

static bool ensure_sized_buf(struct wlr_buffer **buf_out, uint64_t native[2], int *w_stored,
		int *h_stored, int w, int h) {
	if (*buf_out && *w_stored == w && *h_stored == h)
		return native[0] != 0;

	if (*buf_out)
		effects_destroy_buffer(buf_out, native);

	if (!ensure_output_buf(buf_out, native, w, h))
		return false;

	*w_stored = w;
	*h_stored = h;
	return true;
}

bool effects_init(void) {
	effects_state = (effects_state_t){0};
	char *renderer_name = NULL;

	// renderer selector
#ifdef WLR_HAS_GLES2_RENDERER
	if (wlr_renderer_is_gles2(server.renderer)) {
		effects_state.backend = &gles2_backend;
		renderer_name = "gles2";
	}
#endif
#ifdef WLR_HAS_VULKAN_RENDERER
	if (renderer_name == NULL && wlr_renderer_is_vk(server.renderer)) {
		effects_state.backend = &vk_backend;
		renderer_name = "vk";
	}
#endif
	if (renderer_name == NULL) {
		if (wlr_renderer_is_pixman(server.renderer)) {
			wlr_log(WLR_INFO, "effects: pixman renderer detected - blur disabled");
			return false;
		}
		wlr_log(WLR_INFO, "effects: unknown renderer detected - blur disabled");
		return false;
	}

	effects_backend = effects_state.backend;

	if (!effects_state.backend->init(server.renderer, server.allocator)) {
		wlr_log(WLR_INFO, "effects: backend init failed - blur disabled");
		return false;
	}

	const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(server.renderer);
	s_render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
	if (!s_render_fmt)
		s_render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;
	if (!s_render_fmt) {
		wlr_log(WLR_ERROR, "Failed to find a suitable DRM render format");
		effects_state.backend->fini();
		return false;
	}

	effects_state.available = true;
	wlr_log(WLR_INFO, "blur: initialised (%s)", renderer_name);
	return true;
}

void effects_fini(void) {
	if (!effects_state.available)
		return;
	effects_state.backend->fini();
	screen_shader_enabled = false;
	effects_state = (effects_state_t){0};
}

effects_output_t *effects_output_init(int width, int height) {
	if (!effects_state.available)
		return NULL;

	effects_output_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->width = width;
	ctx->height = height;
	int ds = blur_downsample > 0 ? blur_downsample : 1;
	ctx->blur_w = (width / ds) > 0 ? (width / ds) : 1;
	ctx->blur_h = (height / ds) > 0 ? (height / ds) : 1;

	if (!effects_state.backend->output_init(&ctx->be_state, width, height, ctx->blur_w, ctx->blur_h)) {
		free(ctx);
		return NULL;
	}

	pixman_region32_init(&ctx->scratch_region_a);
	pixman_region32_init(&ctx->scratch_region_b);

	if (!create_capture_output(ctx, width, height)) {
		wlr_log(WLR_ERROR, "blur: failed to create capture output");
		pixman_region32_fini(&ctx->scratch_region_a);
		pixman_region32_fini(&ctx->scratch_region_b);
		effects_state.backend->output_fini(&ctx->be_state);
		free(ctx);
		return NULL;
	}

	if (server.shader_tree) {
		ctx->screen_shader_node = wlr_scene_buffer_create(server.shader_tree, NULL);
		if (ctx->screen_shader_node)
			wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
	}

	ctx->mica_dirty = true;
	return ctx;
}

void effects_output_fini(effects_output_t *ctx) {
	if (!ctx)
		return;
	if (effects_state.available)
		effects_state.backend->output_fini(&ctx->be_state);
	effects_destroy_buffer(&ctx->mica_buf, ctx->mica_native);
	effects_destroy_buffer(&ctx->screen_shader_buf, ctx->screen_shader_native);
	if (ctx->screen_shader_node) {
		wlr_scene_node_destroy(&ctx->screen_shader_node->node);
		ctx->screen_shader_node = NULL;
	}
	pixman_region32_fini(&ctx->scratch_region_a);
	pixman_region32_fini(&ctx->scratch_region_b);
	destroy_capture_output(ctx);
	free(ctx);
}

void effects_output_resize(effects_output_t *ctx, int width, int height, output_t *output) {
	if (!ctx || !effects_state.available)
		return;
	int ds = blur_downsample > 0 ? blur_downsample : 1;
	int new_bw = (width / ds) > 0 ? (width / ds) : 1;
	int new_bh = (height / ds) > 0 ? (height / ds) : 1;

	if (ctx->width == width && ctx->height == height && ctx->blur_w == new_bw && ctx->blur_h == new_bh)
		return;

	effects_state.backend->output_resize(&ctx->be_state, width, height, new_bw, new_bh);

	ctx->width = width;
	ctx->height = height;
	ctx->blur_w = new_bw;
	ctx->blur_h = new_bh;
	wlr_output_state_set_custom_mode(&ctx->capture_state, width, height, 0);

	effects_destroy_buffer(&ctx->mica_buf, ctx->mica_native);
	effects_destroy_buffer(&ctx->screen_shader_buf, ctx->screen_shader_native);

	// free per-toplevel blur/acrylic buffers since output dimensions changed
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->blur) {
			effects_destroy_buffer(&tl->blur->blur_buf, tl->blur->blur_native);
			effects_destroy_buffer(&tl->blur->acrylic_buf, tl->blur->acrylic_native);
		}
		if (tl->rounded) {
			effects_destroy_buffer(&tl->rounded->corner_mask_buf, tl->rounded->corner_mask_native);
			effects_destroy_buffer(&tl->rounded->border_shader_buf, tl->rounded->border_shader_native);
			tl->rounded->border_shader_buf_w = 0;
			tl->rounded->border_shader_buf_h = 0;
			tl->rounded->border_dirty = true;
			tl->rounded->corner_mask_dirty = true;
			tl->rounded->border_cache_valid = false;
		}
	}

	// free per-layer-surface blur buffers since output dimensions changed
	if (output) {
		for (int i = 0; i < 4; i++) {
			layer_surface_t *ls;
			wl_list_for_each(ls, &output->layers[i], link)
				effects_destroy_buffer(&ls->blur_buf, ls->blur_native);
		}
	}

	ctx->mica_dirty = true;
}

void effects_invalidate_mica(effects_output_t *ctx) {
	if (ctx)
		ctx->mica_dirty = true;
}

static bool compute_src_box(output_t *output, const struct wlr_box *r, struct wlr_fbox *src_out,
		int *dw_out, int *dh_out) {
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

	if (sx + sw > bw)
		sw = bw - sx;
	if (sy + sh > bh)
		sh = bh - sy;
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

static void hide_workspace_slide_out_tree(output_t *output, node_t *node,
		struct wlr_scene_tree *tree, struct wl_array *hidden_nodes) {
	if (!node || node->output != output || !tree || !tree->node.enabled)
		return;
	if (!animation_node_workspace_slide_out(node))
		return;

	struct wlr_scene_node **hidden = wl_array_add(hidden_nodes, sizeof(*hidden));
	if (!hidden)
		return;

	*hidden = &tree->node;
	wlr_scene_node_set_enabled(*hidden, false);
}

// capture scene with all blur/mica/acrylic toplevel scene trees hidden
static uint64_t capture_bg_to_tex1_ex(output_t *output, effects_output_t *ctx, bool mica_only,
	struct wlr_scene_node *hide_node, bool *hide_flag, bool hide_blur_toplevels,
	bool exclude_slide_out);

static uint64_t capture_bg_to_tex1(output_t *output, effects_output_t *ctx, bool mica_only,
		struct wlr_scene_node *hide_node, bool *hide_flag) {
	return capture_bg_to_tex1_ex(output, ctx, mica_only, hide_node, hide_flag, true, false);
}

static uint64_t capture_bg_to_tex1_ex(output_t *output, effects_output_t *ctx, bool mica_only,
		struct wlr_scene_node *hide_node, bool *hide_flag, bool hide_blur_toplevels,
		bool exclude_slide_out) {
	int w = output->width, h = output->height;
	if (!ctx->capture_output || !ctx->capture_scene_output)
		return 0;
	wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);
	if (w <= 0 || h <= 0)
		return 0;

	struct wl_array hidden_slide_out;
	wl_array_init(&hidden_slide_out);
	if (exclude_slide_out) {
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link)
			hide_workspace_slide_out_tree(output, tl->node, tl->scene_tree, &hidden_slide_out);

		xwayland_toplevel_t *xw;
		wl_list_for_each(xw, &server.xwayland.views, link)
			hide_workspace_slide_out_tree(output, xw->node, xw->scene_tree, &hidden_slide_out);
	}

	if (server.top_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.top_tree->node, false);
	if (server.full_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.full_tree->node, false);
	if (server.over_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.over_tree->node, false);
	if (server.lock_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.lock_tree->node, false);

	if (mica_only) {
		if (server.tile_tree->node.enabled)
			wlr_scene_node_set_enabled(&server.tile_tree->node, false);
		if (server.float_tree->node.enabled)
			wlr_scene_node_set_enabled(&server.float_tree->node, false);
	}

	toplevel_t *tl;
	if (hide_node) {
		*hide_flag = false;
		if (hide_node->enabled) {
			wlr_scene_node_set_enabled(hide_node, false);
			*hide_flag = true;
		}
	} else if (hide_blur_toplevels) {
		wl_list_for_each(tl, &server.toplevels, link) {
			if (!tl->blur)
				continue;
			tl->blur->blur_scene_hidden = false;
			if ((tl->blur->blur_node || tl->blur->mica_node || tl->blur->acrylic_node) && tl->scene_tree &&
					tl->scene_tree->node.enabled) {
				wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
				tl->blur->blur_scene_hidden = true;
			}
		}
	}

	// hide blur layer surfaces
	for (int i = 0; i < 4; i++) {
		layer_surface_t *ls;
		wl_list_for_each(ls, &output->layers[i], link) {
			if (ls->blur_node && ls->mapped) {
				ls->blur_scene_hidden = false;
				if (ls->scene_tree->node.enabled) {
					wlr_scene_node_set_enabled(&ls->scene_tree->node, false);
					ls->blur_scene_hidden = true;
				}
			}
		}
	}

	wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

	bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &ctx->capture_state, NULL);

	if (hide_node) {
		if (*hide_flag)
			wlr_scene_node_set_enabled(hide_node, true);
	} else if (hide_blur_toplevels) {
		wl_list_for_each(tl, &server.toplevels, link)
			if (tl->blur && tl->blur->blur_scene_hidden)
				wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
	}

	// restore blur layer surfaces
	for (int i = 0; i < 4; i++) {
		layer_surface_t *ls;
		wl_list_for_each(ls, &output->layers[i], link)
			if (ls->blur_scene_hidden)
				wlr_scene_node_set_enabled(&ls->scene_tree->node, true);
	}

	struct wlr_scene_node **hidden;
	wl_array_for_each(hidden, &hidden_slide_out)
		wlr_scene_node_set_enabled(*hidden, true);
	wl_array_release(&hidden_slide_out);

	if (!server.top_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.top_tree->node, true);
	if (!server.full_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.full_tree->node, true);
	if (!server.over_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.over_tree->node, true);
	if (!server.lock_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.lock_tree->node, true);
	if (mica_only) {
		if (!server.tile_tree->node.enabled)
			wlr_scene_node_set_enabled(&server.tile_tree->node, true);
		if (!server.float_tree->node.enabled)
			wlr_scene_node_set_enabled(&server.float_tree->node, true);
	}

	wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

	if (!ok || !ctx->capture_state.buffer) {
		wlr_log(WLR_INFO, "capture_bg_to_tex1: no buffer from build_state");
		wlr_buffer_unlock(ctx->capture_state.buffer);
		ctx->capture_state.buffer = NULL;
		return 0;
	}

	uint64_t result;
	effects_backend->capture_readback(ctx->capture_state.buffer, &ctx->be_state,
		ctx->be_state.pong.native_handle[0], ctx->blur_w, ctx->blur_h, w, h, &result);
	wlr_buffer_unlock(ctx->capture_state.buffer);
	ctx->capture_state.buffer = NULL;

	return result;
}

static struct wlr_box get_client_rect(toplevel_t *tl);
static struct wlr_box get_animated_client_rect(toplevel_t *tl);

// captures scene with blur layer surfaces hidden but blur toplevels visible (for layer blur)
static uint64_t capture_bg_combined(output_t *output, effects_output_t *ctx) {
	return capture_bg_to_tex1_ex(output, ctx, false, NULL, NULL, false, false);
}

static bool rebuild_live_blur(output_t *output, uint64_t shared_blurred, bool only_missing) {
	effects_output_t *ctx = output->effects;
	int w = output->width, h = output->height;
	bool any = false;

	struct be_blur_params bp = {
		.algorithm = blur_algorithm,
		.passes = blur_passes,
		.radius = blur_radius,
		.vibrancy = blur_vibrancy,
		.vibrancy_darkness = blur_vibrancy_darkness,
		.noise_strength = blur_noise_strength,
		.brightness = blur_brightness,
		.contrast = blur_contrast,
		.refraction_strength = refraction_strength,
		.refraction_edge_size_px = refraction_edge_size_px,
		.refraction_corner_radius_px = refraction_corner_radius_px,
		.refraction_normal_pow = refraction_normal_pow,
		.refraction_rgb_fringing = refraction_rgb_fringing,
		.refraction_texture_repeat_mode = refraction_texture_repeat_mode,
		.refraction_offset = refraction_offset,
		.refraction_noise_strength = refraction_noise_strength,
		.refraction_noise_scale = refraction_noise_scale,
	};

	// capture shared background if not provided (fallback for non-damaged frames)
	if (!shared_blurred) {
		shared_blurred = capture_bg_to_tex1(output, ctx, false, NULL, NULL);
		if (!shared_blurred) {
			toplevel_t *tl;
			wl_list_for_each(tl, &server.toplevels, link) {
				if (!tl->blur || !tl->blur->blur_node || !tl->node || !tl->node->client)
					continue;
				if (!tl->node->client->shown)
					continue;
				if (!tl->node->output || tl->node->output != output)
					continue;
				if (tl->blur->blur_node)
					wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
			}
			return false;
		}
	}

	// save unblurred background for all toplevels
	{
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (!tl->blur || !tl->blur->blur_node || !tl->node || !tl->node->client)
				continue;
			if (!tl->node->client->shown)
				continue;
			if (!tl->node->output || tl->node->output != output)
				continue;
			if (only_missing && tl->blur->blur_buf)
				continue;

			if (!ensure_output_buf(&tl->blur->blur_buf, tl->blur->blur_native, w, h)) {
				if (tl->blur->blur_node)
					wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
				continue;
			}
			effects_backend->blit(shared_blurred, tl->blur->blur_native[0], w, h, NULL, 0);
		}
	}

	// blur and overlay for each toplevel
	{
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (!tl->blur || !tl->blur->blur_node || !tl->node || !tl->node->client)
				continue;
			if (!tl->node->client->shown)
				continue;
			if (!tl->node->output || tl->node->output != output)
				continue;
			if (only_missing && tl->blur->blur_buf)
				continue;

			uint64_t blurred;
			effects_backend->blur(&ctx->be_state, shared_blurred, ctx->blur_w, ctx->blur_h, &bp, &blurred);

			if (tl->blur && !pixman_region32_empty(&tl->blur->blur_region)) {
				int lx = 0, ly = 0;
				wlr_scene_node_coords(&tl->scene_tree->node, &lx, &ly);
				int nboxes;
				pixman_box32_t *boxes = pixman_region32_rectangles(&tl->blur->blur_region, &nboxes);
				pixman_box32_t scissor_boxes[nboxes];
				for (int b = 0; b < nboxes; b++) {
					scissor_boxes[b].x1 = boxes[b].x1 + lx;
					scissor_boxes[b].y1 = boxes[b].y1 + ly;
					scissor_boxes[b].x2 = boxes[b].x2 + lx;
					scissor_boxes[b].y2 = boxes[b].y2 + ly;
				}
				effects_backend->blit(blurred, tl->blur->blur_native[0], w, h, scissor_boxes, nboxes);
			} else {
				effects_backend->blit(blurred, tl->blur->blur_native[0], w, h, NULL, 0);
			}

			client_t *c = tl->node->client;
			if (c && c->border_radius > 0.0f && c->state != STATE_FULLSCREEN) {
				struct wlr_box content_r = get_animated_client_rect(tl);
				float ow = (float)w, oh = (float)h;
				float win_u = (float)(content_r.x - output->lx) / ow;
				float win_v = (float)(content_r.y - output->ly) / oh;
				float win_sw = (float)content_r.width / ow;
				float win_sh = (float)content_r.height / oh;
				int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;
				float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;
				struct be_corner_mask_params params = {
					.win_u = win_u,
					.win_v = win_v,
					.win_sw = win_sw,
					.win_sh = win_sh,
					.win_size_px_w = (float)content_r.width,
					.win_size_px_h = (float)content_r.height,
					.border_radius_px = inner_r,
					.scale = output->wlr_output->scale,
					.pre_blit = false,
				};
				effects_backend->apply_corner_mask(&ctx->be_state, tl->blur->blur_native[0], w, h, blurred,
					&params);
			}
			any = true;
		}
	}
	return any;
}

static void push_blur_to_toplevels(output_t *output) {
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->blur || !tl->blur->blur_node || !tl->node)
			continue;
		output_t *m = tl->node->output;
		if (!m || m != output)
			continue;

		if (!tl->blur->blur_buf) {
			if (tl->blur->blur_node->buffer)
				wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
			continue;
		}

		if (tl->blur->blur_node->buffer != tl->blur->blur_buf)
			wlr_scene_buffer_set_buffer(tl->blur->blur_node, tl->blur->blur_buf);

		if (tl->blur && !pixman_region32_empty(&tl->blur->blur_region)) {
			int lx = 0, ly = 0;
			wlr_scene_node_coords(&tl->scene_tree->node, &lx, &ly);

			int blur_r_x = tl->blur->blur_region.extents.x1;
			int blur_r_y = tl->blur->blur_region.extents.y1;
			int blur_r_w = tl->blur->blur_region.extents.x2 - tl->blur->blur_region.extents.x1;
			int blur_r_h = tl->blur->blur_region.extents.y2 - tl->blur->blur_region.extents.y1;

			struct wlr_box blur_rect = {
				.x = lx + blur_r_x,
				.y = ly + blur_r_y,
				.width = blur_r_w,
				.height = blur_r_h
			};

			struct wlr_fbox src;
			int dw, dh;
			if (!compute_src_box(output, &blur_rect, &src, &dw, &dh)) {
				wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
				wlr_scene_node_set_position(&tl->blur->blur_node->node, 0, 0);
				continue;
			}

			int offset_x = (blur_rect.x < output->lx) ? (output->lx - blur_rect.x) : 0;
			int offset_y = (blur_rect.y < output->ly) ? (output->ly - blur_rect.y) : 0;
			wlr_scene_node_set_position(&tl->blur->blur_node->node, blur_r_x + offset_x,
				blur_r_y + offset_y);
			wlr_scene_buffer_set_source_box(tl->blur->blur_node, &src);
			wlr_scene_buffer_set_dest_size(tl->blur->blur_node, dw, dh);
		} else {
			struct wlr_box r = get_animated_client_rect(tl);

			struct wlr_fbox src;
			int dw, dh;
			if (!compute_src_box(output, &r, &src, &dw, &dh)) {
				wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
				wlr_scene_node_set_position(&tl->blur->blur_node->node, 0, 0);
				continue;
			}

			int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
			int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
			wlr_scene_node_set_position(&tl->blur->blur_node->node, node_ox, node_oy);
			wlr_scene_buffer_set_source_box(tl->blur->blur_node, &src);
			wlr_scene_buffer_set_dest_size(tl->blur->blur_node, dw, dh);
		}
	}
}

static bool rebuild_live_blur_layers(output_t *output, uint64_t bg_tex, pixman_region32_t *damage) {
	effects_output_t *ctx = output->effects;
	int w = output->width, h = output->height;
	bool any = false;

	pixman_region32_t *overlap_rgn = &ctx->scratch_region_a;
	pixman_region32_t *blur_rgn = &ctx->scratch_region_b;

	struct be_blur_params bp = {
		.algorithm = blur_algorithm,
		.passes = blur_passes,
		.radius = blur_radius,
		.vibrancy = blur_vibrancy,
		.vibrancy_darkness = blur_vibrancy_darkness,
		.noise_strength = blur_noise_strength,
		.brightness = blur_brightness,
		.contrast = blur_contrast,
	};

	if (bg_tex) {
		for (int i = 0; i < 4; i++) {
			layer_surface_t *ls;
			wl_list_for_each(ls, &output->layers[i], link) {
				if (!ls->blur_node || !ls->mapped)
					continue;
				if (pixman_region32_empty(&ls->blur_region))
					continue;
				if (!ensure_output_buf(&ls->blur_buf, ls->blur_native, w, h))
					continue;
				ls->blur_geometry_dirty = true;
				effects_backend->blit(bg_tex, ls->blur_native[0], w, h, NULL, 0);
			}
		}
		for (int i = 0; i < 4; i++) {
			layer_surface_t *ls;
			wl_list_for_each(ls, &output->layers[i], link) {
				if (!ls->blur_node || !ls->mapped)
					continue;

				int lx, ly;
				if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly))
					continue;
				int out_lx = lx - output->lx;
				int out_ly = ly - output->ly;

				if (ls->blur_buf && damage && !pixman_region32_empty(damage)) {
					int nboxes_check;
					pixman_box32_t *boxes_check = pixman_region32_rectangles(&ls->blur_region, &nboxes_check);
					if (nboxes_check > 0) {
						pixman_region32_clear(blur_rgn);
						pixman_region32_union_rect(blur_rgn, blur_rgn, boxes_check[0].x1 + out_lx,
							boxes_check[0].y1 + out_ly, boxes_check[0].x2 - boxes_check[0].x1,
							boxes_check[0].y2 - boxes_check[0].y1);
						for (int b = 1; b < nboxes_check; b++)
							pixman_region32_union_rect(blur_rgn, blur_rgn, boxes_check[b].x1 + out_lx,
								boxes_check[b].y1 + out_ly, boxes_check[b].x2 - boxes_check[b].x1,
								boxes_check[b].y2 - boxes_check[b].y1);
						pixman_region32_intersect(overlap_rgn, damage, blur_rgn);
						if (pixman_region32_empty(overlap_rgn))
							continue;
					}
				}

				int nboxes;
				pixman_box32_t *boxes = pixman_region32_rectangles(&ls->blur_region, &nboxes);
				if (nboxes == 0)
					continue;

				pixman_box32_t out_boxes[nboxes];
				for (int b = 0; b < nboxes; b++) {
					out_boxes[b].x1 = boxes[b].x1 + out_lx;
					out_boxes[b].y1 = boxes[b].y1 + out_ly;
					out_boxes[b].x2 = boxes[b].x2 + out_lx;
					out_boxes[b].y2 = boxes[b].y2 + out_ly;
				}

				uint64_t blurred;
				effects_backend->blur(&ctx->be_state, bg_tex, ctx->blur_w, ctx->blur_h, &bp, &blurred);
				effects_backend->blit(blurred, ls->blur_native[0], w, h, out_boxes, nboxes);
				any = true;
			}
		}
	} else {
		for (int i = 0; i < 4; i++) {
			layer_surface_t *ls;
			wl_list_for_each(ls, &output->layers[i], link) {
				if (!ls->blur_node || !ls->mapped)
					continue;

				int lx, ly;
				if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly))
					continue;
				int out_lx = lx - output->lx;
				int out_ly = ly - output->ly;

				if (ls->blur_buf && damage && !pixman_region32_empty(damage)) {
					int nboxes_check;
					pixman_box32_t *boxes_check = pixman_region32_rectangles(&ls->blur_region, &nboxes_check);
					if (nboxes_check > 0) {
						pixman_region32_clear(blur_rgn);
						pixman_region32_union_rect(blur_rgn, blur_rgn, boxes_check[0].x1 + out_lx,
							boxes_check[0].y1 + out_ly, boxes_check[0].x2 - boxes_check[0].x1,
							boxes_check[0].y2 - boxes_check[0].y1);
						for (int b = 1; b < nboxes_check; b++)
							pixman_region32_union_rect(blur_rgn, blur_rgn, boxes_check[b].x1 + out_lx,
								boxes_check[b].y1 + out_ly, boxes_check[b].x2 - boxes_check[b].x1,
								boxes_check[b].y2 - boxes_check[b].y1);
						pixman_region32_intersect(overlap_rgn, damage, blur_rgn);
						if (pixman_region32_empty(overlap_rgn))
							continue;
					}
				}

				int nboxes;
				pixman_box32_t *boxes = pixman_region32_rectangles(&ls->blur_region, &nboxes);
				if (nboxes == 0)
					continue;

				uint64_t src = capture_bg_to_tex1(output, ctx, false, &ls->scene_tree->node,
					&ls->blur_scene_hidden);
				if (!src)
					continue;

				if (!ensure_output_buf(&ls->blur_buf, ls->blur_native, w, h))
					continue;
				ls->blur_geometry_dirty = true;

				pixman_box32_t out_boxes[nboxes];
				for (int b = 0; b < nboxes; b++) {
					out_boxes[b].x1 = boxes[b].x1 + out_lx;
					out_boxes[b].y1 = boxes[b].y1 + out_ly;
					out_boxes[b].x2 = boxes[b].x2 + out_lx;
					out_boxes[b].y2 = boxes[b].y2 + out_ly;
				}

				effects_backend->blit(src, ls->blur_native[0], w, h, NULL, 0);

				uint64_t blurred;
				effects_backend->blur(&ctx->be_state, src, ctx->blur_w, ctx->blur_h, &bp, &blurred);
				effects_backend->blit(blurred, ls->blur_native[0], w, h, out_boxes, nboxes);
				any = true;
			}
		}
	}
	return any;
}

static void push_blur_to_layers(output_t *output) {
	for (int i = 0; i < 4; i++) {
		layer_surface_t *ls;
		wl_list_for_each(ls, &output->layers[i], link) {
			if (!ls->blur_node)
				continue;

			if (!ls->blur_buf) {
				if (ls->blur_node->buffer)
					wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
				continue;
			}

			// get blur region bounds
			if (ls->blur_region.extents.x2 - ls->blur_region.extents.x1 <= 0 ||
					ls->blur_region.extents.y2 - ls->blur_region.extents.y1 <= 0) {
				if (ls->blur_node->buffer)
					wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
				continue;
			}

			if (!ls->blur_geometry_dirty)
				continue;

			if (ls->blur_node->buffer != ls->blur_buf)
				wlr_scene_buffer_set_buffer(ls->blur_node, ls->blur_buf);

			// get surface position for source box calculation
			int lx, ly;
			if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly)) {
				wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
				continue;
			}

			// position blur_node at the blur region offset within the scene_tree
			int blur_r_x = ls->blur_region.extents.x1;
			int blur_r_y = ls->blur_region.extents.y1;
			int blur_r_w = ls->blur_region.extents.x2 - ls->blur_region.extents.x1;
			int blur_r_h = ls->blur_region.extents.y2 - ls->blur_region.extents.y1;

			// compute the source box in output-local coordinates
			struct wlr_box r = {
				.x = lx + blur_r_x,
				.y = ly + blur_r_y,
				.width = blur_r_w,
				.height = blur_r_h
			};

			struct wlr_fbox src;
			int dw, dh;
			if (!compute_src_box(output, &r, &src, &dw, &dh)) {
				wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
				wlr_scene_node_set_position(&ls->blur_node->node, 0, 0);
				continue;
			}

			int offset_x = (r.x < output->lx) ? (output->lx - r.x) : 0;
			int offset_y = (r.y < output->ly) ? (output->ly - r.y) : 0;

			// position at blur region offset within surface
			wlr_scene_node_set_position(&ls->blur_node->node, blur_r_x + offset_x, blur_r_y + offset_y);
			wlr_scene_buffer_set_source_box(ls->blur_node, &src);
			wlr_scene_buffer_set_dest_size(ls->blur_node, dw, dh);
			ls->blur_geometry_dirty = false;
		}
	}
}

static bool rebuild_live_acrylic(output_t *output, pixman_region32_t *damage,
		uint64_t shared_capture, bool only_missing) {
	effects_output_t *ctx = output->effects;
	int w = output->width, h = output->height;
	bool any = false;

	pixman_region32_t *overlap_rgn = &ctx->scratch_region_a;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->blur || !tl->blur->acrylic_node || !tl->node || !tl->node->client)
			continue;
		if (!tl->node->client->shown)
			continue;
		if (!tl->node->output || tl->node->output != output)
			continue;
		if (only_missing && tl->blur->acrylic_buf)
			continue;

		// skip if toplevel already has a valid acrylic buffer and the damage doesn't overlap it
		if (tl->blur->acrylic_buf && damage && !pixman_region32_empty(damage)) {
			struct wlr_box r = get_animated_client_rect(tl);
			pixman_region32_clear(overlap_rgn);
			pixman_region32_union_rect(overlap_rgn, overlap_rgn, r.x - output->lx, r.y - output->ly, r.width,
				r.height);
			if (pixman_region32_empty(overlap_rgn))
				continue;
		}

		uint64_t src;
		if (shared_capture) {
			src = shared_capture;
		} else {
			src = capture_bg_to_tex1(output, ctx, false, &tl->scene_tree->node,
				&tl->blur->blur_scene_hidden);
			if (!src)
				continue;
		}

		if (!ensure_output_buf(&tl->blur->acrylic_buf, tl->blur->acrylic_native, w, h))
			continue;

		struct be_acrylic_params ap = {
			.tint = {acrylic_tint[0], acrylic_tint[1], acrylic_tint[2], acrylic_tint[3]},
			.tint_strength = acrylic_tint_strength,
			.noise_strength = acrylic_noise_strength,
			.res_w = (float)w,
			.res_h = (float)h,
			.light_anchor_x = acrylic_light_anchor[0],
			.light_anchor_y = acrylic_light_anchor[1],
			.blur_passes = acrylic_blur_passes,
			.blur_radius = blur_radius,
		};
		effects_backend->apply_acrylic(&ctx->be_state, src, &ap, tl->blur->acrylic_native[0], w, h);

		client_t *c = tl->node->client;
		if (c && c->border_radius > 0.0f && c->state != STATE_FULLSCREEN) {
			struct wlr_box content_r = get_animated_client_rect(tl);
			float ow = (float)w, oh = (float)h;
			float win_u = (float)(content_r.x - output->lx) / ow;
			float win_v = (float)(content_r.y - output->ly) / oh;
			float win_sw = (float)content_r.width / ow;
			float win_sh = (float)content_r.height / oh;
			int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;
			float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;

			struct be_corner_mask_params params = {
				.win_u = win_u,
				.win_v = win_v,
				.win_sw = win_sw,
				.win_sh = win_sh,
				.win_size_px_w = (float)content_r.width,
				.win_size_px_h = (float)content_r.height,
				.border_radius_px = inner_r,
				.scale = output->wlr_output->scale,
				.pre_blit = true,
			};
			effects_backend->apply_corner_mask(&ctx->be_state, tl->blur->acrylic_native[0], w, h, src,
				&params);
		}

		any = true;
	}
	return any;
}

static void push_acrylic_to_toplevels(output_t *output) {
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->blur || !tl->blur->acrylic_node || !tl->node)
			continue;
		output_t *m = tl->node->output;
		if (!m || m != output)
			continue;

		if (!tl->blur->acrylic_buf) {
			if (tl->blur->acrylic_node->buffer)
				wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, NULL);
			continue;
		}

		if (tl->blur->acrylic_node->buffer != tl->blur->acrylic_buf)
			wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, tl->blur->acrylic_buf);

		struct wlr_box r = get_animated_client_rect(tl);

		struct wlr_fbox src;
		int dw, dh;
		if (!compute_src_box(output, &r, &src, &dw, &dh)) {
			wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, NULL);
			wlr_scene_node_set_position(&tl->blur->acrylic_node->node, 0, 0);
			continue;
		}

		int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
		int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
		wlr_scene_node_set_position(&tl->blur->acrylic_node->node, node_ox, node_oy);
		wlr_scene_buffer_set_source_box(tl->blur->acrylic_node, &src);
		wlr_scene_buffer_set_dest_size(tl->blur->acrylic_node, dw, dh);
	}
}

static bool rebuild_mica(output_t *output, uint64_t pre_captured_bg) {
	effects_output_t *ctx = output->effects;
	int w = output->width, h = output->height;

	uint64_t src;
	if (pre_captured_bg) {
		src = pre_captured_bg;
	} else {
		src = capture_bg_to_tex1(output, ctx, true, NULL, NULL);
		if (!src) {
			ctx->mica_dirty = false;
			return false;
		}
	}

	struct be_blur_params bp = {
		.algorithm = blur_algorithm,
		.passes = blur_passes,
		.radius = blur_radius,
		.vibrancy = blur_vibrancy,
		.vibrancy_darkness = blur_vibrancy_darkness,
		.noise_strength = blur_noise_strength,
		.brightness = blur_brightness,
		.contrast = blur_contrast,
	};
	uint64_t blurred;
	effects_backend->blur(&ctx->be_state, src, ctx->blur_w, ctx->blur_h, &bp, &blurred);

	if (!ensure_output_buf(&ctx->mica_buf, ctx->mica_native, w, h)) {
		ctx->mica_dirty = false;
		return false;
	}

	effects_backend->apply_mica_tint(&ctx->be_state, blurred, mica_tint, mica_tint_strength,
		ctx->mica_native[0], w, h);

	ctx->mica_dirty = false;
	return true;
}

static void push_mica_to_toplevels(output_t *output) {
	struct wlr_buffer *buf = output->effects->mica_buf;
	if (!buf)
		return;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->blur || !tl->blur->mica_node || !tl->node)
			continue;
		output_t *m = tl->node->output;
		if (!m || m != output)
			continue;

		if (tl->blur->mica_node->buffer != buf)
			wlr_scene_buffer_set_buffer(tl->blur->mica_node, buf);

		struct wlr_box r = get_animated_client_rect(tl);

		struct wlr_fbox src;
		int dw, dh;
		if (!compute_src_box(output, &r, &src, &dw, &dh)) {
			wlr_scene_buffer_set_buffer(tl->blur->mica_node, NULL);
			wlr_scene_node_set_position(&tl->blur->mica_node->node, 0, 0);
			continue;
		}

		int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
		int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
		wlr_scene_node_set_position(&tl->blur->mica_node->node, node_ox, node_oy);
		wlr_scene_buffer_set_source_box(tl->blur->mica_node, &src);
		wlr_scene_buffer_set_dest_size(tl->blur->mica_node, dw, dh);
	}
}

static struct wlr_box get_client_rect(toplevel_t *tl) {
	client_t *c = tl->node->client;
	if (c->state == STATE_FULLSCREEN && tl->node->output)
		return tl->node->output->rectangle;
	else if (c->state == STATE_FLOATING)
		return c->floating_rectangle;
	else
		return c->tiled_rectangle;
}

// interpolate client rect with active animation progress (resize, move, or slide)
static struct wlr_box get_animated_client_rect(toplevel_t *tl) {
	struct wlr_box r = get_client_rect(tl);
	struct wlr_box anim;
	if (animation_get_geometry_progress(tl, &anim)) {
		r.x = anim.x;
		r.y = anim.y;
		r.width = anim.width;
		r.height = anim.height;
	}
	return r;
}

static bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

static bool blur_render_shadow(toplevel_t *tl) {
	if (!tl->shadow)
		return false;
	if (!tl->node || !tl->node->client)
		return false;

	client_t *c = tl->node->client;
	if (!c->shadow)
		return false;
	if (c->state == STATE_FULLSCREEN)
		return false;

	struct wlr_box client_r = get_animated_client_rect(tl);
	if (client_r.width <= 0 || client_r.height <= 0)
		return false;

	int size = (int)shadow_size;
	if (size <= 0)
		return false;
	int buf_w = client_r.width + 2 * size;
	int buf_h = client_r.height + 2 * size;
	if (buf_w <= 0 || buf_h <= 0)
		return false;

	if (!tl->shadow->shadow_node) {
		tl->shadow->shadow_node = wlr_scene_buffer_create(tl->scene_tree, NULL);
		if (!tl->shadow->shadow_node)
			return false;
		wlr_scene_node_lower_to_bottom(&tl->shadow->shadow_node->node);
		tl->shadow->shadow_node->point_accepts_input = scene_buffer_no_input;
	}

	float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
	double phys_buf_w = buf_w * scale;
	double phys_buf_h = buf_h * scale;

	if (!ensure_sized_buf(&tl->shadow->shadow_buf, tl->shadow->shadow_native, &tl->shadow->shadow_buf_w,
		&tl->shadow->shadow_buf_h, (int)phys_buf_w, (int)phys_buf_h))
		return false;

	struct be_shadow_params sp = {
		.shadow_size = size * scale,
		.shadow_offset_x = shadow_offset_x,
		.shadow_offset_y = shadow_offset_y,
		.shadow_color = {c->shadow_color[0], c->shadow_color[1], c->shadow_color[2], c->shadow_color[3]},
		.border_radius = c->border_radius * scale,
		.inner_width = client_r.width * scale,
		.inner_height = client_r.height * scale,
		.hole_x = (tl->content_tree->node.x - shadow_offset_x + size) * scale,
		.hole_y = (tl->content_tree->node.y - shadow_offset_y + size) * scale,
		.hole_width = client_r.width * scale,
		.hole_height = client_r.height * scale,
		.scale = scale,
		.buf_w = (int)phys_buf_w,
		.buf_h = (int)phys_buf_h,
	};
	effects_backend->render_shadow(&sp, tl->shadow->shadow_native[0]);

	wlr_scene_node_lower_to_bottom(&tl->shadow->shadow_node->node);
	if (tl->shadow->shadow_node->buffer != tl->shadow->shadow_buf)
		wlr_scene_buffer_set_buffer(tl->shadow->shadow_node, tl->shadow->shadow_buf);
	struct wlr_fbox src_box = {
		0,
		0,
		phys_buf_w,
		phys_buf_h
	};
	wlr_scene_buffer_set_source_box(tl->shadow->shadow_node, &src_box);
	wlr_scene_buffer_set_dest_size(tl->shadow->shadow_node, buf_w, buf_h);

	wlr_scene_node_set_position(&tl->shadow->shadow_node->node, shadow_offset_x - size,
		shadow_offset_y - size);
	wlr_scene_node_set_enabled(&tl->shadow->shadow_node->node, true);

	return true;
}

static bool blur_render_border(toplevel_t *tl, int content_w, int content_h) {
	if (!tl->border_tree)
		return false;
	if (!tl->rounded)
		return false;

	float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
	client_t *c = tl->node->client;
	int bw_i = border_width;
	if (bw_i <= 0)
		return false;

	double log_fw = (double)content_w + 2 * bw_i;
	double log_fh = (double)content_h + 2 * bw_i;
	if (log_fw <= 0 || log_fh <= 0)
		return false;

	int phys_w = (int)(log_fw * scale + 0.5);
	int phys_h = (int)(log_fh * scale + 0.5);
	if (phys_w <= 0 || phys_h <= 0)
		return false;

	if (!tl->rounded->border_shader_node) {
		tl->rounded->border_shader_node = wlr_scene_buffer_create(tl->border_tree, NULL);
		if (!tl->rounded->border_shader_node)
			return false;
		wlr_scene_node_set_position(&tl->rounded->border_shader_node->node, 0, 0);
		tl->rounded->border_shader_node->point_accepts_input = scene_buffer_no_input;
	}

	if (!ensure_sized_buf(&tl->rounded->border_shader_buf, tl->rounded->border_shader_native,
		&tl->rounded->border_shader_buf_w, &tl->rounded->border_shader_buf_h, phys_w, phys_h))
		return false;

	float render_scale_x = (float)phys_w / (float)log_fw;
	float render_scale_y = (float)phys_h / (float)log_fh;
	float render_scale = (render_scale_x + render_scale_y) * 0.5f;

	struct be_border_params bp;
	memset(&bp, 0, sizeof(bp));
	bp.res_w = (float)log_fw;
	bp.res_h = (float)log_fh;
	bp.border_radius = (float)c->border_radius;
	bp.border_width_px = (float)bw_i;
	bp.scale = render_scale;
	memcpy(bp.border_color, tl->rounded->border_color, sizeof(bp.border_color));
	memcpy(bp.gradient_colors, tl->rounded->gradient_colors, sizeof(bp.gradient_colors));
	bp.gradient_count = tl->rounded->gradient_count;
	bp.gradient_angle = tl->rounded->gradient_angle;
	memcpy(bp.gradient2_colors, tl->rounded->gradient2_colors, sizeof(bp.gradient2_colors));
	bp.gradient2_count = tl->rounded->gradient2_count;
	bp.gradient2_angle = tl->rounded->gradient2_angle;
	bp.gradient_lerp = tl->rounded->gradient_lerp;
	bp.buf_w = phys_w;
	bp.buf_h = phys_h;

	if (!tl->rounded->border_cache_valid ||
			tl->rounded->border_shader_buf != tl->rounded->cached_border_buf || memcmp(&bp,
			&tl->rounded->cached_border_params, sizeof(bp)) != 0) {
		effects_backend->render_border(&bp, tl->rounded->border_shader_native[0]);
		tl->rounded->cached_border_params = bp;
		tl->rounded->cached_border_buf = tl->rounded->border_shader_buf;
		tl->rounded->border_cache_valid = true;
	}

	if (tl->rounded->border_shader_node->buffer != tl->rounded->border_shader_buf)
		wlr_scene_buffer_set_buffer(tl->rounded->border_shader_node, tl->rounded->border_shader_buf);
	struct wlr_fbox src_box = {
		0,
		0,
		(float)phys_w,
		(float)phys_h
	};
	wlr_scene_buffer_set_source_box(tl->rounded->border_shader_node, &src_box);
	wlr_scene_buffer_set_dest_size(tl->rounded->border_shader_node, (int)log_fw, (int)log_fh);
	wlr_scene_node_set_enabled(&tl->rounded->border_shader_node->node, true);

	static const float transparent[4] = {
		0.0f,
		0.0f,
		0.0f,
		0.0f
	};
	for (int i = 0; i < 4; i++)
		if (tl->border_rects[i])
			wlr_scene_rect_set_color(tl->border_rects[i], transparent);

	return true;
}

static bool rebuild_corner_masks(output_t *output, uint64_t bg_tex) {
	effects_output_t *ctx = output->effects;
	int w = output->width, h = output->height;
	bool any = false;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->rounded || !tl->rounded->corner_mask_node || !tl->node || !tl->node->client)
			continue;
		if (!tl->node->client->shown)
			continue;
		if (!tl->node->output || tl->node->output != output)
			continue;

		client_t *c = tl->node->client;
		if (c->border_radius <= 0.0f || c->state == STATE_FULLSCREEN)
			continue;

		struct wlr_box container_r = get_animated_client_rect(tl);
		int cx = tl->content_tree->node.x;
		int cy = tl->content_tree->node.y;
		int surf_w = (tl->geometry.width > 0 &&
			tl->geometry.width < container_r.width) ? (int)tl->geometry.width : container_r.width;
		int surf_h = (tl->geometry.height > 0 &&
			tl->geometry.height < container_r.height) ? (int)tl->geometry.height : container_r.height;
		struct wlr_box content_r = {
			.x = container_r.x + cx,
			.y = container_r.y + cy,
			.width = surf_w,
			.height = surf_h,
		};
		if (content_r.width <= 0 || content_r.height <= 0)
			continue;

		uint64_t src;
		if (bg_tex) {
			src = bg_tex;
		} else {
			int cw = output->width, ch = output->height;

			wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);
			if (server.top_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.top_tree->node, false);
			if (server.full_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.full_tree->node, false);
			if (server.over_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.over_tree->node, false);
			if (server.lock_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.lock_tree->node, false);

			struct {
				struct wlr_scene_node *node;
				bool was;
			} restore[8];
			int nr = 0;

#define HIDE_IF(n)                                                                                                     \
	do {                                                                                                                 \
		if ((n) && (n)->enabled) {                                                                                         \
			restore[nr].node = (n);                                                                                          \
			restore[nr].was = true;                                                                                          \
			wlr_scene_node_set_enabled((n), false);                                                                          \
			nr++;                                                                                                            \
		}                                                                                                                  \
	} while (0)

			HIDE_IF(&tl->content_tree->node);
			HIDE_IF(&tl->border_tree->node);
			if (tl->blur) {
				HIDE_IF(&tl->blur->blur_node->node);
				HIDE_IF(&tl->blur->mica_node->node);
				HIDE_IF(&tl->blur->acrylic_node->node);
			}
			if (tl->rounded)
				HIDE_IF(&tl->rounded->corner_mask_node->node);

			wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);
			struct wlr_output_state cap_state;
			wlr_output_state_init(&cap_state);
			wlr_output_state_set_enabled(&cap_state, true);
			wlr_output_state_set_custom_mode(&cap_state, cw, ch, 0);
			bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);

			for (int i = 0; i < nr; i++)
				wlr_scene_node_set_enabled(restore[i].node, true);

#undef HIDE_IF

			if (!server.top_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.top_tree->node, true);
			if (!server.full_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.full_tree->node, true);
			if (!server.over_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.over_tree->node, true);
			if (!server.lock_tree->node.enabled)
				wlr_scene_node_set_enabled(&server.lock_tree->node, true);
			wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

			if (!ok || !cap_state.buffer) {
				wlr_output_state_finish(&cap_state);
				tl->rounded->corner_mask_dirty = false;
				continue;
			}

			effects_backend->capture_readback(cap_state.buffer, &ctx->be_state,
				ctx->be_state.pong.native_handle[0], ctx->blur_w, ctx->blur_h, cw, ch, &src);
			wlr_output_state_finish(&cap_state);
			if (!src) {
				tl->rounded->corner_mask_dirty = false;
				continue;
			}
		}

		if (!ensure_output_buf(&tl->rounded->corner_mask_buf, tl->rounded->corner_mask_native, w, h)) {
			tl->rounded->corner_mask_dirty = false;
			continue;
		}

		float ow = (float)w, oh = (float)h;
		int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;
		float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;

		float win_u = (float)(content_r.x - output->lx) / ow;
		float win_v = (float)(content_r.y - output->ly) / oh;
		float win_sw = (float)content_r.width / ow;
		float win_sh = (float)content_r.height / oh;

		struct be_corner_mask_params params = {
			.out_w = w,
			.out_h = h,
			.win_u = win_u,
			.win_v = win_v,
			.win_sw = win_sw,
			.win_sh = win_sh,
			.win_size_px_w = (float)content_r.width,
			.win_size_px_h = (float)content_r.height,
			.border_radius_px = inner_r,
			.scale = output->wlr_output->scale,
			.pre_blit = true,
		};

		// blit background to dest, then apply inverse corner mask
		effects_backend->blit(src, tl->rounded->corner_mask_native[0], w, h, NULL, 0);
		effects_backend->apply_corner_mask(&ctx->be_state, tl->rounded->corner_mask_native[0], w, h, src,
			&params);

		tl->rounded->corner_mask_dirty = false;
		any = true;
	}
	return any;
}

static void push_corner_masks_to_toplevels(output_t *output, bool rebuilt) {
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->rounded || !tl->rounded->corner_mask_node || !tl->node || !tl->node->client)
			continue;
		output_t *m = tl->node->output;
		if (!m || m != output)
			continue;

		client_t *c = tl->node->client;
		if (c->border_radius <= 0.0f || c->state == STATE_FULLSCREEN) {
			wlr_scene_buffer_set_buffer(tl->rounded->corner_mask_node, NULL);
			continue;
		}
		if (!tl->rounded->corner_mask_buf)
			continue;

		// when corner masks weren't rebuilt, only ensure the node is enabled
		// (avoid touching the scene node's buffer/size/position on every frame)
		if (!rebuilt) {
			if (!tl->rounded->corner_mask_node->node.enabled)
				wlr_scene_node_set_enabled(&tl->rounded->corner_mask_node->node, true);
			continue;
		}

		struct wlr_box content_r = get_animated_client_rect(tl);

		struct wlr_fbox src;
		int dw, dh;
		if (!compute_src_box(output, &content_r, &src, &dw, &dh))
			continue;

		int node_ox = (content_r.x < output->lx) ? (output->lx - content_r.x) : 0;
		int node_oy = (content_r.y < output->ly) ? (output->ly - content_r.y) : 0;

		if (!tl->rounded->corner_mask_node->node.enabled)
			wlr_scene_node_set_enabled(&tl->rounded->corner_mask_node->node, true);
		if (tl->rounded->corner_mask_node->buffer != tl->rounded->corner_mask_buf)
			wlr_scene_buffer_set_buffer(tl->rounded->corner_mask_node, tl->rounded->corner_mask_buf);
		wlr_scene_node_set_position(&tl->rounded->corner_mask_node->node, node_ox, node_oy);
		wlr_scene_buffer_set_source_box(tl->rounded->corner_mask_node, &src);
		wlr_scene_buffer_set_dest_size(tl->rounded->corner_mask_node, dw, dh);
	}
}

static uint64_t capture_full_scene_to_tex(output_t *output, effects_output_t *ctx) {
	int w = output->width, h = output->height;
	if (!ctx->capture_output || !ctx->capture_scene_output)
		return 0;
	if (w <= 0 || h <= 0)
		return 0;

	uint64_t screen_fbo = ctx->be_state.screen_shader.native_handle[0];
	if (!screen_fbo)
		return 0;

	wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);

	// hide the shader overlay to avoid a feedback loop
	if (server.shader_tree && server.shader_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.shader_tree->node, false);

	wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

	bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &ctx->capture_state, NULL);

	if (server.shader_tree && !server.shader_tree->node.enabled)
		wlr_scene_node_set_enabled(&server.shader_tree->node, true);

	wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

	if (!ok || !ctx->capture_state.buffer) {
		wlr_buffer_unlock(ctx->capture_state.buffer);
		ctx->capture_state.buffer = NULL;
		return 0;
	}

	uint64_t result;
	effects_backend->capture_readback(ctx->capture_state.buffer, &ctx->be_state, screen_fbo, w, h, w, h,
		&result);

	wlr_buffer_unlock(ctx->capture_state.buffer);
	ctx->capture_state.buffer = NULL;
	return result;
}

static void handle_screen_shader_frame(output_t *output) {
	effects_output_t *ctx = output->effects;
	if (!ctx || !ctx->screen_shader_node)
		return;

	if (!screen_shader_enabled) {
		wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
		return;
	}

	int w = output->width, h = output->height;
	if (w <= 0 || h <= 0)
		return;

	uint64_t src = capture_full_scene_to_tex(output, ctx);
	if (!src) {
		wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
		return;
	}

	if (!ensure_output_buf(&ctx->screen_shader_buf, ctx->screen_shader_native, w, h)) {
		wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	// the screen shader program and time tracking are managed by the backend
	struct be_screen_shader_params ssp = {
		.type = SCREEN_SHADER_CUSTOM,
		.time = (float)(now.tv_sec) + (float)(now.tv_nsec) * 1e-9f,
		.scale = output->wlr_output->scale
	};
	effects_backend->apply_screen_shader(src, ctx->screen_shader_native[0], w, h, &ssp);

	wlr_scene_buffer_set_buffer(ctx->screen_shader_node, ctx->screen_shader_buf);
	struct wlr_fbox src_box = {
		0,
		0,
		(double)w,
		(double)h
	};
	wlr_scene_buffer_set_source_box(ctx->screen_shader_node, &src_box);
	wlr_scene_buffer_set_dest_size(ctx->screen_shader_node, w, h);
	wlr_scene_node_set_position(&ctx->screen_shader_node->node, output->lx, output->ly);
	wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, true);
}

void effects_evict_buffers(void) {
	if (!effects_state.available)
		return;

	if (wl_list_empty(&server.toplevels))
		return;

	effects_state.eviction_counter++;
	if (effects_state.eviction_counter % 10 != 0)
		return;

	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		bool visible = tl->node && tl->node->client && tl->node->client->shown;

		if (tl->blur) {
			// visual eviction
			if (tl->blur->blur_node && !blur_enabled)
				wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
			if (tl->blur->acrylic_node && !blur_enabled)
				wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, NULL);
			if (tl->blur->mica_node && !mica_enabled)
				wlr_scene_buffer_set_buffer(tl->blur->mica_node, NULL);

			// memory eviction
			if (tl->blur->blur_buf && !visible)
				effects_destroy_buffer(&tl->blur->blur_buf, tl->blur->blur_native);
			if (tl->blur->acrylic_buf && !visible)
				effects_destroy_buffer(&tl->blur->acrylic_buf, tl->blur->acrylic_native);
		}

		// only evict from hidden toplevels
		if (tl->rounded) {
			if (tl->rounded->corner_mask_buf && !visible) {
				tl->rounded->corner_mask_dirty = true;
				effects_destroy_buffer(&tl->rounded->corner_mask_buf, tl->rounded->corner_mask_native);
			}
			if (tl->rounded->border_shader_buf && !visible) {
				if (tl->rounded->border_shader_node)
					wlr_scene_buffer_set_buffer(tl->rounded->border_shader_node, NULL);
				effects_destroy_buffer(&tl->rounded->border_shader_buf, tl->rounded->border_shader_native);
				tl->rounded->border_shader_buf_w = 0;
				tl->rounded->border_shader_buf_h = 0;
				tl->rounded->border_cache_valid = false;
			}
		}
	}
}

static bool damage_in_tree(pixman_region32_t *damage, struct wlr_scene_tree *tree) {
	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(damage, &nrects);
	for (int i = 0; i < nrects; i++) {
		double cx = (rects[i].x1 + rects[i].x2) * 0.5;
		double cy = (rects[i].y1 + rects[i].y2) * 0.5;
		double nx, ny;
		if (wlr_scene_node_at(&tree->node, cx, cy, &nx, &ny))
			return true;
	}
	return false;
}

static bool background_capture_needed(struct wlr_scene_output *scene_output) {
	pixman_region32_t *damage = &scene_output->damage_ring.current;
	if (pixman_region32_empty(damage))
		return false;

	// only check trees not hidden during background capture
	struct wlr_scene_tree *relevant[] = {
		server.bg_tree,
		server.bot_tree,
		server.tile_tree,
		server.float_tree,
		server.shader_tree,
		server.drag_tree,
	};

	for (size_t i = 0; i < sizeof(relevant) / sizeof(relevant[0]); i++)
		if (damage_in_tree(damage, relevant[i]))
			return true;

	return false;
}

static bool workspace_effect_buffers_missing(output_t *output) {
	toplevel_t *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (!tl->blur || !tl->node || !tl->node->client)
			continue;
		if (!tl->node->client->shown || tl->node->output != output)
			continue;

		if (blur_enabled && tl->blur->blur_node && !tl->blur->blur_buf)
			return true;
		if (tl->blur->acrylic_node && !tl->blur->acrylic_buf)
			return true;
	}

	return false;
}

void effects_output_frame(output_t *output, struct wlr_scene_output *scene_output) {
	if (!effects_state.available)
		return;
	effects_output_t *ctx = output->effects;
	if (!ctx)
		return;

	effects_evict_buffers();

	bool workspace_switch = animation_workspace_switch_active(output);
	bool workspace_warmup = workspace_switch && workspace_effect_buffers_missing(output);

	// check if layer blur surfaces need rendering
	bool has_layer_blur = false;
	if (blur_enabled) {
		for (int i = 0; i < 4 && !has_layer_blur; i++) {
			layer_surface_t *ls;
			wl_list_for_each(ls, &output->layers[i], link)
				if (ls->blur_node && ls->mapped) {
					has_layer_blur = true;
				break;
			}
		}
	}

	// check if corner masks need rendering
	bool any_cm = false;
	bool any_cm_dirty = false;
	{
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (tl->rounded && tl->rounded->corner_mask_node && tl->node && tl->node->client &&
					tl->node->client->border_radius > 0.0f && tl->node->client->state != STATE_FULLSCREEN &&
					tl->node->output && tl->node->output == output) {
				any_cm = true;
				if (tl->rounded->corner_mask_dirty)
					any_cm_dirty = true;
			}
		}
	}

	// layer blur and corner masks still need updating each frame during the slide.
	if (workspace_switch && !workspace_warmup) {
		if (!has_layer_blur && !any_cm)
			return;
		goto layer_only_frame;
	}

	if (ctx->width != output->width || ctx->height != output->height)
		effects_output_resize(ctx, output->width, output->height, output);

	effects_backend->frame_begin();

	bool bg_damaged = workspace_warmup || background_capture_needed(scene_output);
	bool mica_dirty = mica_enabled && ctx->mica_dirty;

	// when no relevant damage and mica not dirty, skip all effects work
	if (!bg_damaged && !mica_dirty)
		goto after_capture;

	{
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (!tl->shadow || (!tl->shadow->shadow_dirty && !tl->shadow->shadow_geometry_dirty))
				continue;
			if (!tl->node || !tl->node->client || !tl->node->client->shown)
				continue;
			if (!tl->node->output || tl->node->output != output)
				continue;

			if (!tl->node->client->shadow) {
				tl->shadow->shadow_dirty = false;
				tl->shadow->shadow_geometry_dirty = false;
				if (tl->shadow->shadow_node && tl->shadow->shadow_node->node.enabled)
					wlr_scene_node_set_enabled(&tl->shadow->shadow_node->node, false);
				continue;
			}

			if (tl->shadow->shadow_geometry_dirty && !tl->shadow->shadow_dirty) {
				int size = (int)shadow_size;
				if (tl->shadow->shadow_node && size > 0) {
					struct wlr_box client_r = get_client_rect(tl);
					int buf_w = client_r.width + 2 * size;
					int buf_h = client_r.height + 2 * size;
					if (buf_w > 0 && buf_h > 0) {
						float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
						if (tl->shadow->shadow_buf_w == (int)(buf_w * scale) &&
								tl->shadow->shadow_buf_h == (int)(buf_h * scale)) {
							wlr_scene_node_lower_to_bottom(&tl->shadow->shadow_node->node);
							wlr_scene_node_set_position(&tl->shadow->shadow_node->node, shadow_offset_x - size,
								shadow_offset_y - size);
							if (!tl->shadow->shadow_node->node.enabled)
								wlr_scene_node_set_enabled(&tl->shadow->shadow_node->node, true);
							tl->shadow->shadow_geometry_dirty = false;
							continue;
						}
					}
				}
			}

			blur_render_shadow(tl);
			tl->shadow->shadow_dirty = false;
			tl->shadow->shadow_geometry_dirty = false;
		}
	}

	// capture background once for sharing across blur/acrylic/effects
	uint64_t shared_bg = 0;
	{
		bool needs_bg = false;
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (!tl->node || !tl->node->client || !tl->node->client->shown)
				continue;
			if (!tl->node->output || tl->node->output != output)
				continue;
			if (tl->blur && (tl->blur->blur_node || tl->blur->acrylic_node)) {
				needs_bg = true;
				break;
			}
		}
		if (needs_bg)
			shared_bg = capture_bg_to_tex1_ex(output, ctx, false, NULL, NULL, true, workspace_warmup);
	}

	// toplevel blur
	if (blur_enabled) {
		bool any_blur = false;
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (tl->blur && tl->blur->blur_node && tl->node && tl->node->client && tl->node->client->shown &&
					tl->node->output && tl->node->output == output) {
				any_blur = true;
				break;
			}
		}
		if (any_blur) {
			rebuild_live_blur(output, shared_bg, workspace_warmup);
			push_blur_to_toplevels(output);
		}
	}

	// apply acrylic (before layer blur / corner masks so shared_bg in pong is still valid)
	{
		bool any_acrylic = false;
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (tl->blur && tl->blur->acrylic_node && tl->node && tl->node->client &&
					tl->node->client->shown && tl->node->output && tl->node->output == output) {
				any_acrylic = true;
				break;
			}
		}
		if (any_acrylic) {
			rebuild_live_acrylic(output, &scene_output->damage_ring.current, shared_bg, workspace_warmup);
			push_acrylic_to_toplevels(output);
		}
	}

	// apply corner masks and blur if needed
	uint64_t cm_bg_tex = 0;
	if (has_layer_blur) {
		bool any_layer_needs_blur = false;
		if (blur_enabled) {
			pixman_region32_t *damage = &scene_output->damage_ring.current;
			for (int i = 0; i < 4 && !any_layer_needs_blur; i++) {
				layer_surface_t *ls;
				wl_list_for_each(ls, &output->layers[i], link) {
					if (!ls->blur_node || !ls->mapped)
						continue;
					if (!ls->blur_buf) {
						any_layer_needs_blur = true;
						break;
					}
					if (pixman_region32_empty(damage))
						continue;
					int nboxes;
					pixman_box32_t *boxes = pixman_region32_rectangles(&ls->blur_region, &nboxes);
					if (nboxes == 0)
						continue;
					int lx, ly;
					if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly))
						continue;
					int out_lx = lx - output->lx, out_ly = ly - output->ly;
					pixman_region32_clear(&ctx->scratch_region_a);
					pixman_region32_union_rect(&ctx->scratch_region_a, &ctx->scratch_region_a, boxes[0].x1 + out_lx,
						boxes[0].y1 + out_ly, boxes[0].x2 - boxes[0].x1, boxes[0].y2 - boxes[0].y1);
					for (int b = 1; b < nboxes; b++)
						pixman_region32_union_rect(&ctx->scratch_region_a, &ctx->scratch_region_a,
							boxes[b].x1 + out_lx, boxes[b].y1 + out_ly, boxes[b].x2 - boxes[b].x1,
							boxes[b].y2 - boxes[b].y1);

					pixman_region32_intersect(&ctx->scratch_region_b, damage, &ctx->scratch_region_a);
					if (!pixman_region32_empty(&ctx->scratch_region_b))
						any_layer_needs_blur = true;
				}
			}
		}

		if (any_layer_needs_blur && any_cm) {
			uint64_t bg_tex = capture_bg_combined(output, ctx);
			if (bg_tex && blur_enabled) {
				rebuild_live_blur_layers(output, bg_tex, &scene_output->damage_ring.current);
				push_blur_to_layers(output);
			} else if (blur_enabled) {
				rebuild_live_blur_layers(output, 0, &scene_output->damage_ring.current);
				push_blur_to_layers(output);
			}
			if (any_cm_dirty)
				cm_bg_tex = capture_bg_to_tex1(output, ctx, true, NULL, NULL);
			if (any_cm_dirty)
				rebuild_corner_masks(output, cm_bg_tex);
			push_corner_masks_to_toplevels(output, any_cm_dirty);
		} else if (any_layer_needs_blur) {
			rebuild_live_blur_layers(output, 0, &scene_output->damage_ring.current);
			push_blur_to_layers(output);
		} else if (any_cm) {
			if (any_cm_dirty)
				cm_bg_tex = capture_bg_to_tex1(output, ctx, true, NULL, NULL);
			if (any_cm_dirty)
				rebuild_corner_masks(output, cm_bg_tex);
			push_corner_masks_to_toplevels(output, any_cm_dirty);
		}
	} else if (any_cm) {
		if (any_cm_dirty)
			cm_bg_tex = capture_bg_to_tex1(output, ctx, true, NULL, NULL);
		if (any_cm_dirty)
			rebuild_corner_masks(output, cm_bg_tex);
		push_corner_masks_to_toplevels(output, any_cm_dirty);
	}

	if (mica_dirty)
		rebuild_mica(output, cm_bg_tex);

	if (mica_enabled && ctx->mica_buf)
		push_mica_to_toplevels(output);

after_capture:

	// shader border
	{
		toplevel_t *tl;
		wl_list_for_each(tl, &server.toplevels, link) {
			if (!tl->rounded || !tl->rounded->border_dirty)
				continue;
			if (!tl->node || !tl->node->client || !tl->node->client->shown)
				continue;
			if (!tl->node->output || tl->node->output != output)
				continue;
			client_t *c = tl->node->client;

			// use shader if rounded corners or a gradient is set
			bool has_gradient = (tl->rounded->gradient_count >= 2);
			if (c->border_radius <= 0.0f && !has_gradient) {
				tl->rounded->border_dirty = false;
				tl->rounded->corner_mask_dirty = false;
				continue;
			}

			struct wlr_box content_r = get_client_rect(tl);

			if ((c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED) && tl->geometry.width > 0 &&
					tl->geometry.height > 0) {
				if ((int)tl->geometry.width < content_r.width)
					content_r.width = tl->geometry.width;
				if ((int)tl->geometry.height < content_r.height)
					content_r.height = tl->geometry.height;
			}

			blur_render_border(tl, content_r.width, content_r.height);
			tl->rounded->border_dirty = false;
			tl->rounded->corner_mask_dirty = false;
		}
	}

	if (screen_shader_enabled)
		handle_screen_shader_frame(output);

	effects_backend->frame_end();
	return;

layer_only_frame:
	if (ctx->width != output->width || ctx->height != output->height)
		effects_output_resize(ctx, output->width, output->height, output);

	effects_backend->frame_begin();

	// apply corner masks and layer blur if needed
	uint64_t cm_bg_tex_lo = 0;
	if (has_layer_blur) {
		bool any_layer_needs_blur = false;
		if (blur_enabled) {
			pixman_region32_t *damage = &scene_output->damage_ring.current;
			for (int i = 0; i < 4 && !any_layer_needs_blur; i++) {
				layer_surface_t *ls;
				wl_list_for_each(ls, &output->layers[i], link) {
					if (!ls->blur_node || !ls->mapped)
						continue;
					if (!ls->blur_buf) {
						any_layer_needs_blur = true;
						break;
					}
					if (pixman_region32_empty(damage))
						continue;
					int nboxes;
					pixman_box32_t *boxes = pixman_region32_rectangles(&ls->blur_region, &nboxes);
					if (nboxes == 0)
						continue;
					int lx, ly;
					if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly))
						continue;
					int out_lx = lx - output->lx, out_ly = ly - output->ly;
					pixman_region32_clear(&ctx->scratch_region_a);
					pixman_region32_union_rect(&ctx->scratch_region_a, &ctx->scratch_region_a, boxes[0].x1 + out_lx,
						boxes[0].y1 + out_ly, boxes[0].x2 - boxes[0].x1, boxes[0].y2 - boxes[0].y1);
					for (int b = 1; b < nboxes; b++)
						pixman_region32_union_rect(&ctx->scratch_region_a, &ctx->scratch_region_a,
							boxes[b].x1 + out_lx, boxes[b].y1 + out_ly, boxes[b].x2 - boxes[b].x1,
							boxes[b].y2 - boxes[b].y1);
					pixman_region32_intersect(&ctx->scratch_region_b, damage, &ctx->scratch_region_a);
					if (!pixman_region32_empty(&ctx->scratch_region_b))
						any_layer_needs_blur = true;
				}
			}
		}

		if (any_layer_needs_blur && any_cm) {
			uint64_t bg_tex = capture_bg_combined(output, ctx);
			if (bg_tex && blur_enabled) {
				rebuild_live_blur_layers(output, bg_tex, &scene_output->damage_ring.current);
				push_blur_to_layers(output);
			} else if (blur_enabled) {
				rebuild_live_blur_layers(output, 0, &scene_output->damage_ring.current);
				push_blur_to_layers(output);
			}
			goto cm_bg_tex_lo_cap;
		} else if (any_layer_needs_blur) {
			rebuild_live_blur_layers(output, 0, &scene_output->damage_ring.current);
			push_blur_to_layers(output);
		} else if (any_cm)
			goto cm_bg_tex_lo_cap;
	} else if (any_cm) {
	cm_bg_tex_lo_cap:
		if (any_cm_dirty) {
			cm_bg_tex_lo = capture_bg_to_tex1(output, ctx, true, NULL, NULL);
			rebuild_corner_masks(output, cm_bg_tex_lo);
		}
		push_corner_masks_to_toplevels(output, any_cm_dirty);
	}

	effects_backend->frame_end();
}

enum blur_algorithm blur_algorithm_from_str(const char *str) {
	if (!str)
		return BLUR_ALGORITHM_KAWASE;
	if (strcmp(str, "kawase") == 0)
		return BLUR_ALGORITHM_KAWASE;
	if (strcmp(str, "gaussian") == 0)
		return BLUR_ALGORITHM_GAUSSIAN;
	if (strcmp(str, "box") == 0)
		return BLUR_ALGORITHM_BOX;
	if (strcmp(str, "refraction") == 0)
		return BLUR_ALGORITHM_REFRACTION;
	if (strcmp(str, "lens_refraction") == 0)
		return BLUR_ALGORITHM_LENS_REFRACTION;
	if (strcmp(str, "none") == 0)
		return BLUR_ALGORITHM_NONE;
	wlr_log(WLR_ERROR, "blur: unknown algorithm '%s', using kawase", str);
	return BLUR_ALGORITHM_KAWASE;
}

const char *effects_algorithm_to_str(enum blur_algorithm algo) {
	switch (algo) {
	case BLUR_ALGORITHM_KAWASE:
		return "kawase";
	case BLUR_ALGORITHM_GAUSSIAN:
		return "gaussian";
	case BLUR_ALGORITHM_BOX:
		return "box";
	case BLUR_ALGORITHM_REFRACTION:
		return "refraction";
	case BLUR_ALGORITHM_LENS_REFRACTION:
		return "lens_refraction";
	default:
		return "none";
	}
}

bool screen_shader_set(const char *name) {
	if (!effects_state.available)
		return false;
	if (!name || strcmp(name, "none") == 0) {
		screen_shader_clear();
		return true;
	}

	const char *frag_src = effects_backend->get_screen_shader_source(name);

	if (!frag_src)
		return false;

	effects_backend->frame_begin();
	bool ok = effects_backend->compile_screen_shader(frag_src);
	if (ok) {
		screen_shader_enabled = true;
		snprintf(screen_shader_name_str, sizeof(screen_shader_name_str), "%s", name);
	}
	effects_backend->frame_end();
	return ok;
}

bool screen_shader_load_file(const char *path) {
	if (!effects_state.available || !path)
		return false;

	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_ERROR, "screen_shader: cannot open '%s'", path);
		return false;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);

	if (size <= 0 || size > 1024 * 1024) {
		fclose(f);
		wlr_log(WLR_ERROR, "screen_shader: file '%s' too large or empty", path);
		return false;
	}

	char *src = malloc((size_t)size + 1);
	if (!src) {
		fclose(f);
		return false;
	}

	size_t nread = fread(src, 1, (size_t)size, f);
	fclose(f);
	src[nread] = '\0';

	effects_backend->frame_begin();
	bool ok = effects_backend->compile_screen_shader(src);
	free(src);
	if (ok) {
		screen_shader_enabled = true;
		snprintf(screen_shader_name_str, sizeof(screen_shader_name_str), "%s", path);
	}
	effects_backend->frame_end();
	return ok;
}

void screen_shader_clear(void) {
	if (effects_state.available) {
		effects_backend->frame_begin();
		effects_backend->destroy_screen_shader();
		effects_backend->frame_end();
	}
	screen_shader_enabled = false;
	snprintf(screen_shader_name_str, sizeof(screen_shader_name_str), "none");
}

const char *screen_shader_get_name(void) {
	return screen_shader_name_str;
}
