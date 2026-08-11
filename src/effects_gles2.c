#include "effects_backend.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#ifdef WLR_HAS_GLES2_RENDERER
const struct wlr_drm_format_set *wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

#include "gl_blit_frag_src.h"
#include "gl_blur_acrylic_frag_src.h"
#include "gl_blur_box_h_frag_src.h"
#include "gl_blur_box_v_frag_src.h"
#include "gl_blur_gauss_h_frag_src.h"
#include "gl_blur_gauss_v_frag_src.h"
#include "gl_blur_kawase_frag_src.h"
#include "gl_blur_down_frag_src.h"
#include "gl_blur_up_frag_src.h"
#include "gl_blur_mica_frag_src.h"
#include "gl_blur_refraction_frag_src.h"
#include "gl_border_corner_mask_frag_src.h"
#include "gl_border_frag_src.h"
#include "gl_effect_tex_vert_src.h"
#include "gl_ext_blit_frag_src.h"
#include "gl_grayscale_frag_src.h"
#include "gl_invert_frag_src.h"
#include "gl_nightlight_frag_src.h"
#include "gl_sepia_frag_src.h"
#include "gl_shadow_frag_src.h"

struct gles2_data {
	EGLDisplay egl_display;
	EGLContext egl_context;

	GLuint prog_kawase;
	GLuint prog_blur_down;
	GLuint prog_blur_up;
	GLuint prog_gauss_h, prog_gauss_v;
	GLuint prog_box_h, prog_box_v;
	GLuint prog_blit;
	GLuint prog_mica_tint;
	GLuint prog_acrylic_tint;
	GLuint prog_refraction;
	GLuint prog_ext_blit;
	GLuint prog_border;
	GLuint prog_corner_mask;
	GLuint prog_shadow;

	struct {
		GLint tex, halfpixel, offset, noise_strength, vibrancy, vibrancy_darkness, brightness, contrast;
	} u_kawase;
	struct {
		GLint tex, halfpixel, offset;
	} u_blur_down;
	struct {
		GLint tex, halfpixel, offset, adjust, saturation, noise_strength, vibrancy, vibrancy_darkness,
			brightness, contrast;
	} u_blur_up;
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
		GLint resolution, border_radius, border_width_px, border_color, scale;
		GLint gradient_colors, gradient_count, gradient_angle;
		GLint gradient2_colors, gradient2_count, gradient2_angle;
		GLint gradient_lerp;
	} u_border;
	struct {
		GLint tex, win_pos_uv, win_size_uv, win_size_px, border_radius_px, scale, bg_pos_uv, bg_size_uv;
	} u_corner_mask;
	struct {
		GLint resolution, shadow_size, shadow_color, border_radius, inner_size, hole_pos, hole_size;
	} u_shadow;
	uint8_t *readback_pixels;
	size_t readback_pixels_size;

	GLuint screen_shader_prog;
	GLint screen_shader_u_tex;
	GLint screen_shader_u_resolution;
	GLint screen_shader_u_time;

	GLuint vbo;
	GLint attr_pos;

	const struct wlr_drm_format *render_fmt;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
};

static struct gles2_data *g = NULL;

static bool egl_make_current(void) {
	return eglMakeCurrent(g->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->egl_context) == EGL_TRUE;
}

static void egl_unset_current(void) {
	eglMakeCurrent(g->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static GLuint compile_shader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		GLsizei len = 0;
		glGetShaderInfoLog(s, sizeof(log), &len, log);
		const char *shader_type = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
		wlr_log(WLR_ERROR, "gles2: %s shader compilation error: %.*s", shader_type, len, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint link_program(const char *frag_src) {
	GLuint vert = compile_shader(GL_VERTEX_SHADER, gl_effect_tex_vert_src);
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
		char log[1024];
		GLsizei len = 0;
		glGetProgramInfoLog(prog, sizeof(log), &len, log);
		wlr_log(WLR_ERROR, "gles2: program linking error: %.*s", len, log);
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
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "gles2: FBO incomplete (0x%x)", status);
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

static GLuint gles2_ensure_blur_level(be_output_state_t *state, int i, int w, int h) {
	if (w <= 0 || h <= 0)
		return 0;
	be_buffer_t *lv = &state->blur_levels[i];
	if (lv->native_handle[0] && lv->width == w && lv->height == h)
		return (GLuint)lv->native_handle[0];
	if (lv->native_handle[0])
		destroy_fbo((GLuint *)&lv->native_handle[0], (GLuint *)&lv->native_handle[1]);
	if (!create_fbo(w, h, (GLuint *)&lv->native_handle[0], (GLuint *)&lv->native_handle[1]))
		return 0;
	lv->width = w;
	lv->height = h;
	return (GLuint)lv->native_handle[0];
}

static void gles2_destroy_blur_levels(be_output_state_t *state) {
	for (int i = 0; i < BLUR_MAX_LEVELS; i++)
		destroy_fbo((GLuint *)&state->blur_levels[i].native_handle[0],
			(GLuint *)&state->blur_levels[i].native_handle[1]);
	memset(state->blur_levels, 0, sizeof(state->blur_levels));
}

static void draw_quad(void) {
	glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
	glEnableVertexAttribArray(g->attr_pos);
	glVertexAttribPointer(g->attr_pos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(g->attr_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// draw the full-screen quad once, or once per scissor box (y-flipped to GL coords).
static void draw_quad_scissored(const pixman_box32_t *scissor, int n_scissor, int h) {
	if (n_scissor > 0 && scissor) {
		glEnable(GL_SCISSOR_TEST);
		for (int i = 0; i < n_scissor; i++) {
			glScissor(scissor[i].x1, h - scissor[i].y2, scissor[i].x2 - scissor[i].x1,
				scissor[i].y2 - scissor[i].y1);
			draw_quad();
		}
		glDisable(GL_SCISSOR_TEST);
	} else {
		draw_quad();
	}
}

static void blur_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, int pass_index,
		struct be_blur_params *p, const pixman_box32_t *scissor, int n_scissor) {
	glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);

	glUseProgram(g->prog_kawase);
	glUniform1i(g->u_kawase.tex, 0);
	glUniform2f(g->u_kawase.halfpixel, 0.5f / (float)w, 0.5f / (float)h);
	glUniform1f(g->u_kawase.offset, p->radius * (float)(pass_index + 1));
	if (g->u_kawase.noise_strength >= 0)
		glUniform1f(g->u_kawase.noise_strength, p->noise_strength);
	if (g->u_kawase.vibrancy >= 0)
		glUniform1f(g->u_kawase.vibrancy, p->vibrancy);
	if (g->u_kawase.vibrancy_darkness >= 0)
		glUniform1f(g->u_kawase.vibrancy_darkness, p->vibrancy_darkness);
	if (g->u_kawase.brightness >= 0)
		glUniform1f(g->u_kawase.brightness, p->brightness);
	if (g->u_kawase.contrast >= 0)
		glUniform1f(g->u_kawase.contrast, p->contrast);

	draw_quad_scissored(scissor, n_scissor, h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void refraction_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, struct be_blur_params *p,
		int refraction_mode, const pixman_box32_t *scissor, int n_scissor) {
	glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);

	glUseProgram(g->prog_refraction);
	glUniform1i(g->u_refraction.tex, 0);

	if (g->u_refraction.offset >= 0)
		glUniform1f(g->u_refraction.offset, p->refraction_offset);
	if (g->u_refraction.halfpixel >= 0)
		glUniform2f(g->u_refraction.halfpixel, 0.5f / (float)w, 0.5f / (float)h);
	if (g->u_refraction.refraction_rect_size >= 0)
		glUniform2f(g->u_refraction.refraction_rect_size, (float)w, (float)h);

	float max_edge = 0.5f * (float)((w < h) ? w : h);
	float edge = p->refraction_edge_size_px;
	if (edge > max_edge)
		edge = max_edge;
	if (edge < 0.0f)
		edge = 0.0f;
	if (g->u_refraction.refraction_edge_size_pixels >= 0)
		glUniform1f(g->u_refraction.refraction_edge_size_pixels, edge);

	float max_corner = 0.5f * (float)((w < h) ? w : h);
	float corner = p->refraction_corner_radius_px;
	if (corner > max_corner)
		corner = max_corner;
	if (corner < 0.0f)
		corner = 0.0f;
	if (g->u_refraction.refraction_corner_radius_pixels >= 0)
		glUniform1f(g->u_refraction.refraction_corner_radius_pixels, corner);

	float strength_norm = p->refraction_strength / 30.0f;
	if (strength_norm < 0.0f)
		strength_norm = 0.0f;
	if (strength_norm > 1.0f)
		strength_norm = 1.0f;
	if (g->u_refraction.refraction_strength >= 0)
		glUniform1f(g->u_refraction.refraction_strength, strength_norm);

	if (g->u_refraction.refraction_normal_pow >= 0)
		glUniform1f(g->u_refraction.refraction_normal_pow, p->refraction_normal_pow);

	float fringing = p->refraction_rgb_fringing;
	if (fringing < 0.0f)
		fringing = 0.0f;
	if (fringing > 1.0f)
		fringing = 1.0f;
	if (g->u_refraction.refraction_RGB_fringing >= 0)
		glUniform1f(g->u_refraction.refraction_RGB_fringing, fringing);

	if (g->u_refraction.refraction_texture_repeat_mode >= 0)
		glUniform1i(g->u_refraction.refraction_texture_repeat_mode, p->refraction_texture_repeat_mode);

	if (g->u_refraction.refraction_mode >= 0)
		glUniform1i(g->u_refraction.refraction_mode, refraction_mode);

	draw_quad_scissored(scissor, n_scissor, h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void box_pass_dir(GLuint prog, GLuint src_tex, GLuint dst_fbo, int w, int h,
		struct be_blur_params *p, const pixman_box32_t *scissor, int n_scissor) {
	glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);
	glUseProgram(prog);
	glUniform1i(g->u_box.tex, 0);
	glUniform2f(g->u_box.texel_size, 1.0f / w, 1.0f / h);
	glUniform1f(g->u_box.radius, p->radius);
	if (g->u_box.vibrancy >= 0)
		glUniform1f(g->u_box.vibrancy, p->vibrancy);
	if (g->u_box.vibrancy_darkness >= 0)
		glUniform1f(g->u_box.vibrancy_darkness, p->vibrancy_darkness);
	if (g->u_box.brightness >= 0)
		glUniform1f(g->u_box.brightness, p->brightness);
	if (g->u_box.contrast >= 0)
		glUniform1f(g->u_box.contrast, p->contrast);
	draw_quad_scissored(scissor, n_scissor, h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void box_pass(GLuint src_tex, GLuint ping_fbo, GLuint ping_tex, GLuint pong_fbo, int w, int h,
		struct be_blur_params *p, const pixman_box32_t *scissor, int n_scissor) {
	box_pass_dir(g->prog_box_h, src_tex, ping_fbo, w, h, p, scissor, n_scissor);
	box_pass_dir(g->prog_box_v, ping_tex, pong_fbo, w, h, p, scissor, n_scissor);
}

static void gaussian_pass_dir(GLuint prog, GLuint src_tex, GLuint dst_fbo, int w, int h,
		struct be_blur_params *p, const pixman_box32_t *scissor, int n_scissor) {
	glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);
	glUseProgram(prog);
	glUniform1i(g->u_gauss.tex, 0);
	glUniform2f(g->u_gauss.texel_size, 1.0f / w, 1.0f / h);
	glUniform1f(g->u_gauss.radius, p->radius);
	if (g->u_gauss.vibrancy >= 0)
		glUniform1f(g->u_gauss.vibrancy, p->vibrancy);
	if (g->u_gauss.vibrancy_darkness >= 0)
		glUniform1f(g->u_gauss.vibrancy_darkness, p->vibrancy_darkness);
	if (g->u_gauss.brightness >= 0)
		glUniform1f(g->u_gauss.brightness, p->brightness);
	if (g->u_gauss.contrast >= 0)
		glUniform1f(g->u_gauss.contrast, p->contrast);
	draw_quad_scissored(scissor, n_scissor, h);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void gaussian_pass(GLuint src_tex, GLuint ping_fbo, GLuint ping_tex, GLuint pong_fbo, int w,
		int h, struct be_blur_params *p, const pixman_box32_t *scissor, int n_scissor) {
	gaussian_pass_dir(g->prog_gauss_h, src_tex, ping_fbo, w, h, p, scissor, n_scissor);
	gaussian_pass_dir(g->prog_gauss_v, ping_tex, pong_fbo, w, h, p, scissor, n_scissor);
}

static bool gles2_init(struct wlr_renderer *r, struct wlr_allocator *a) {
	g = calloc(1, sizeof(struct gles2_data));
	if (!g)
		return false;

	if (!wlr_renderer_is_gles2(r)) {
		wlr_log(WLR_INFO, "gles2: renderer is not GLES2");
		free(g);
		g = NULL;
		return false;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(r);
	g->egl_display = wlr_egl_get_display(egl);
	g->egl_context = wlr_egl_get_context(egl);
	g->renderer = r;
	g->allocator = a;

	{
		const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(r);
		g->render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
		if (!g->render_fmt)
			g->render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;
		if (!g->render_fmt) {
			wlr_log(WLR_ERROR, "gles2: Failed to find a suitable DRM render format");
			free(g);
			g = NULL;
			return false;
		}
	}

	if (!egl_make_current()) {
		wlr_log(WLR_ERROR, "gles2: failed to make EGL context current");
		free(g);
		g = NULL;
		return false;
	}

	g->prog_kawase = link_program(gl_blur_kawase_frag_src);
	g->prog_blur_down = link_program(gl_blur_down_frag_src);
	g->prog_blur_up = link_program(gl_blur_up_frag_src);
	g->prog_gauss_h = link_program(gl_blur_gauss_h_frag_src);
	g->prog_gauss_v = link_program(gl_blur_gauss_v_frag_src);
	g->prog_box_h = link_program(gl_blur_box_h_frag_src);
	g->prog_box_v = link_program(gl_blur_box_v_frag_src);
	g->prog_blit = link_program(gl_blit_frag_src);
	g->prog_mica_tint = link_program(gl_blur_mica_frag_src);
	g->prog_acrylic_tint = link_program(gl_blur_acrylic_frag_src);
	g->prog_refraction = link_program(gl_blur_refraction_frag_src);
	g->prog_ext_blit = link_program(gl_ext_blit_frag_src);
	g->prog_border = link_program(gl_border_frag_src);
	g->prog_corner_mask = link_program(gl_border_corner_mask_frag_src);
	g->prog_shadow = link_program(gl_shadow_frag_src);

	if (!g->prog_kawase || !g->prog_blur_down || !g->prog_blur_up || !g->prog_gauss_h ||
			!g->prog_gauss_v || !g->prog_box_h || !g->prog_box_v || !g->prog_blit || !g->prog_mica_tint ||
			!g->prog_acrylic_tint || !g->prog_refraction) {
		wlr_log(WLR_ERROR, "gles2: one or more required shaders failed to compile");
		egl_unset_current();
		free(g);
		g = NULL;
		return false;
	}

	g->u_kawase.tex = glGetUniformLocation(g->prog_kawase, "tex");
	g->u_kawase.halfpixel = glGetUniformLocation(g->prog_kawase, "halfpixel");
	g->u_kawase.offset = glGetUniformLocation(g->prog_kawase, "offset");
	g->u_kawase.noise_strength = glGetUniformLocation(g->prog_kawase, "noise_strength");
	g->u_kawase.vibrancy = glGetUniformLocation(g->prog_kawase, "vibrancy");
	g->u_kawase.vibrancy_darkness = glGetUniformLocation(g->prog_kawase, "vibrancy_darkness");
	g->u_kawase.brightness = glGetUniformLocation(g->prog_kawase, "brightness");
	g->u_kawase.contrast = glGetUniformLocation(g->prog_kawase, "contrast");

	g->u_blur_down.tex = glGetUniformLocation(g->prog_blur_down, "tex");
	g->u_blur_down.halfpixel = glGetUniformLocation(g->prog_blur_down, "halfpixel");
	g->u_blur_down.offset = glGetUniformLocation(g->prog_blur_down, "offset");

	g->u_blur_up.tex = glGetUniformLocation(g->prog_blur_up, "tex");
	g->u_blur_up.halfpixel = glGetUniformLocation(g->prog_blur_up, "halfpixel");
	g->u_blur_up.offset = glGetUniformLocation(g->prog_blur_up, "offset");
	g->u_blur_up.adjust = glGetUniformLocation(g->prog_blur_up, "adjust");
	g->u_blur_up.saturation = glGetUniformLocation(g->prog_blur_up, "saturation");
	g->u_blur_up.noise_strength = glGetUniformLocation(g->prog_blur_up, "noise_strength");
	g->u_blur_up.vibrancy = glGetUniformLocation(g->prog_blur_up, "vibrancy");
	g->u_blur_up.vibrancy_darkness = glGetUniformLocation(g->prog_blur_up, "vibrancy_darkness");
	g->u_blur_up.brightness = glGetUniformLocation(g->prog_blur_up, "brightness");
	g->u_blur_up.contrast = glGetUniformLocation(g->prog_blur_up, "contrast");

	g->u_gauss.tex = glGetUniformLocation(g->prog_gauss_h, "tex");
	g->u_gauss.texel_size = glGetUniformLocation(g->prog_gauss_h, "texel_size");
	g->u_gauss.radius = glGetUniformLocation(g->prog_gauss_h, "radius");
	g->u_gauss.vibrancy = glGetUniformLocation(g->prog_gauss_h, "vibrancy");
	g->u_gauss.vibrancy_darkness = glGetUniformLocation(g->prog_gauss_h, "vibrancy_darkness");
	g->u_gauss.brightness = glGetUniformLocation(g->prog_gauss_h, "brightness");
	g->u_gauss.contrast = glGetUniformLocation(g->prog_gauss_h, "contrast");

	g->u_box.tex = glGetUniformLocation(g->prog_box_h, "tex");
	g->u_box.texel_size = glGetUniformLocation(g->prog_box_h, "texel_size");
	g->u_box.radius = glGetUniformLocation(g->prog_box_h, "radius");
	g->u_box.vibrancy = glGetUniformLocation(g->prog_box_h, "vibrancy");
	g->u_box.vibrancy_darkness = glGetUniformLocation(g->prog_box_h, "vibrancy_darkness");
	g->u_box.brightness = glGetUniformLocation(g->prog_box_h, "brightness");
	g->u_box.contrast = glGetUniformLocation(g->prog_box_h, "contrast");

	g->u_blit.tex = glGetUniformLocation(g->prog_blit, "tex");

	if (g->prog_ext_blit)
		g->u_ext_blit.tex = glGetUniformLocation(g->prog_ext_blit, "tex");

	g->u_mica.tex = glGetUniformLocation(g->prog_mica_tint, "tex");
	g->u_mica.tint = glGetUniformLocation(g->prog_mica_tint, "tint");
	g->u_mica.tint_strength = glGetUniformLocation(g->prog_mica_tint, "tint_strength");

	g->u_acrylic.tex = glGetUniformLocation(g->prog_acrylic_tint, "tex");
	g->u_acrylic.tint = glGetUniformLocation(g->prog_acrylic_tint, "tint");
	g->u_acrylic.tint_strength = glGetUniformLocation(g->prog_acrylic_tint, "tint_strength");
	g->u_acrylic.noise_strength = glGetUniformLocation(g->prog_acrylic_tint, "noise_strength");
	g->u_acrylic.resolution = glGetUniformLocation(g->prog_acrylic_tint, "resolution");
	g->u_acrylic.light_anchor = glGetUniformLocation(g->prog_acrylic_tint, "light_anchor");

	g->u_refraction.tex = glGetUniformLocation(g->prog_refraction, "tex");
	g->u_refraction.offset = glGetUniformLocation(g->prog_refraction, "offset");
	g->u_refraction.halfpixel = glGetUniformLocation(g->prog_refraction, "halfpixel");
	g->u_refraction.refraction_rect_size = glGetUniformLocation(g->prog_refraction,
		"refraction_rect_size");
	g->u_refraction.refraction_edge_size_pixels = glGetUniformLocation(g->prog_refraction,
		"refraction_edge_size_pixels");
	g->u_refraction.refraction_corner_radius_pixels = glGetUniformLocation(g->prog_refraction,
		"refraction_corner_radius_pixels");
	g->u_refraction.refraction_strength = glGetUniformLocation(g->prog_refraction,
		"refraction_strength");
	g->u_refraction.refraction_normal_pow = glGetUniformLocation(g->prog_refraction,
		"refraction_normal_pow");
	g->u_refraction.refraction_RGB_fringing = glGetUniformLocation(g->prog_refraction,
		"refraction_RGB_fringing");
	g->u_refraction.refraction_texture_repeat_mode = glGetUniformLocation(g->prog_refraction,
		"refraction_texture_repeat_mode");
	g->u_refraction.refraction_mode = glGetUniformLocation(g->prog_refraction, "refraction_mode");

	if (g->prog_border) {
		g->u_border.resolution = glGetUniformLocation(g->prog_border, "resolution");
		g->u_border.border_radius = glGetUniformLocation(g->prog_border, "border_radius");
		g->u_border.border_width_px = glGetUniformLocation(g->prog_border, "border_width_px");
		g->u_border.border_color = glGetUniformLocation(g->prog_border, "border_color");
		g->u_border.scale = glGetUniformLocation(g->prog_border, "scale");
		g->u_border.gradient_colors = glGetUniformLocation(g->prog_border, "gradient_colors");
		g->u_border.gradient_count = glGetUniformLocation(g->prog_border, "gradient_count");
		g->u_border.gradient_angle = glGetUniformLocation(g->prog_border, "gradient_angle");
		g->u_border.gradient2_colors = glGetUniformLocation(g->prog_border, "gradient2_colors");
		g->u_border.gradient2_count = glGetUniformLocation(g->prog_border, "gradient2_count");
		g->u_border.gradient2_angle = glGetUniformLocation(g->prog_border, "gradient2_angle");
		g->u_border.gradient_lerp = glGetUniformLocation(g->prog_border, "gradient_lerp");
	}
	if (g->prog_corner_mask) {
		g->u_corner_mask.tex = glGetUniformLocation(g->prog_corner_mask, "tex");
		g->u_corner_mask.win_pos_uv = glGetUniformLocation(g->prog_corner_mask, "win_pos_uv");
		g->u_corner_mask.win_size_uv = glGetUniformLocation(g->prog_corner_mask, "win_size_uv");
		g->u_corner_mask.win_size_px = glGetUniformLocation(g->prog_corner_mask, "win_size_px");
		g->u_corner_mask.border_radius_px = glGetUniformLocation(g->prog_corner_mask, "border_radius_px");
		g->u_corner_mask.scale = glGetUniformLocation(g->prog_corner_mask, "scale");
		g->u_corner_mask.bg_pos_uv = glGetUniformLocation(g->prog_corner_mask, "bg_pos_uv");
		g->u_corner_mask.bg_size_uv = glGetUniformLocation(g->prog_corner_mask, "bg_size_uv");
	}
	if (g->prog_shadow) {
		g->u_shadow.resolution = glGetUniformLocation(g->prog_shadow, "resolution");
		g->u_shadow.shadow_size = glGetUniformLocation(g->prog_shadow, "shadow_size");
		g->u_shadow.shadow_color = glGetUniformLocation(g->prog_shadow, "shadow_color");
		g->u_shadow.border_radius = glGetUniformLocation(g->prog_shadow, "border_radius");
		g->u_shadow.inner_size = glGetUniformLocation(g->prog_shadow, "inner_size");
		g->u_shadow.hole_pos = glGetUniformLocation(g->prog_shadow, "hole_pos");
		g->u_shadow.hole_size = glGetUniformLocation(g->prog_shadow, "hole_size");
	}

	g->attr_pos = 0;

	static const float quad[] = {
		-1.0f,
		-1.0f,
		1.0f,
		-1.0f,
		-1.0f,
		1.0f,
		1.0f,
		1.0f,
	};
	glGenBuffers(1, &g->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	egl_unset_current();
	wlr_log(WLR_INFO, "gles2: initialised");
	return true;
}

static void gles2_fini(void) {
	if (!g)
		return;
	egl_make_current();
	glDeleteProgram(g->prog_kawase);
	glDeleteProgram(g->prog_gauss_h);
	glDeleteProgram(g->prog_gauss_v);
	glDeleteProgram(g->prog_box_h);
	glDeleteProgram(g->prog_box_v);
	glDeleteProgram(g->prog_blit);
	glDeleteProgram(g->prog_mica_tint);
	glDeleteProgram(g->prog_acrylic_tint);
	glDeleteProgram(g->prog_refraction);
	if (g->prog_ext_blit)
		glDeleteProgram(g->prog_ext_blit);
	if (g->prog_border)
		glDeleteProgram(g->prog_border);
	if (g->prog_corner_mask)
		glDeleteProgram(g->prog_corner_mask);
	if (g->prog_shadow)
		glDeleteProgram(g->prog_shadow);
	if (g->screen_shader_prog)
		glDeleteProgram(g->screen_shader_prog);
	glDeleteBuffers(1, &g->vbo);
	free(g->readback_pixels);
	egl_unset_current();
	g->screen_shader_prog = 0;
	free(g);
	g = NULL;
}

static bool gles2_output_init(be_output_state_t *state, int width, int height, int blur_w,
		int blur_h) {
	egl_make_current();
	bool ok = create_fbo(blur_w, blur_h, (GLuint *)&state->ping.native_handle[0],
		(GLuint *)&state->ping.native_handle[1]) && create_fbo(blur_w, blur_h,
		(GLuint *)&state->pong.native_handle[0], (GLuint *)&state->pong.native_handle[1]) &&
		create_fbo(blur_w, blur_h, (GLuint *)&state->blur_scratch.native_handle[0],
			(GLuint *)&state->blur_scratch.native_handle[1]);
	state->ping.width = blur_w;
	state->ping.height = blur_h;
	state->pong.width = blur_w;
	state->pong.height = blur_h;
	state->blur_scratch.width = blur_w;
	state->blur_scratch.height = blur_h;

	// Staging texture (no FBO needed)
	create_fbo(width, height, (GLuint *)&state->staging.native_handle[0],
		(GLuint *)&state->staging.native_handle[1]);
	state->staging.width = width;
	state->staging.height = height;

	if (!create_fbo(width, height, (GLuint *)&state->screen_shader.native_handle[0],
		(GLuint *)&state->screen_shader.native_handle[1]))
		wlr_log(WLR_ERROR, "gles2: screen shader FBO creation failed (non-fatal)");
	state->screen_shader.width = width;
	state->screen_shader.height = height;

	egl_unset_current();
	return ok;
}

static void gles2_output_fini(be_output_state_t *state) {
	if (!g)
		return;
	egl_make_current();
	destroy_fbo((GLuint *)&state->ping.native_handle[0], (GLuint *)&state->ping.native_handle[1]);
	destroy_fbo((GLuint *)&state->pong.native_handle[0], (GLuint *)&state->pong.native_handle[1]);
	destroy_fbo((GLuint *)&state->blur_scratch.native_handle[0],
		(GLuint *)&state->blur_scratch.native_handle[1]);
	destroy_fbo((GLuint *)&state->screen_shader.native_handle[0],
		(GLuint *)&state->screen_shader.native_handle[1]);
	destroy_fbo((GLuint *)&state->staging.native_handle[0],
		(GLuint *)&state->staging.native_handle[1]);
	gles2_destroy_blur_levels(state);
	egl_unset_current();
}

static void gles2_output_resize(be_output_state_t *state, int width, int height, int blur_w,
		int blur_h) {
	egl_make_current();
	destroy_fbo((GLuint *)&state->ping.native_handle[0], (GLuint *)&state->ping.native_handle[1]);
	destroy_fbo((GLuint *)&state->pong.native_handle[0], (GLuint *)&state->pong.native_handle[1]);
	destroy_fbo((GLuint *)&state->blur_scratch.native_handle[0],
		(GLuint *)&state->blur_scratch.native_handle[1]);
	destroy_fbo((GLuint *)&state->screen_shader.native_handle[0],
		(GLuint *)&state->screen_shader.native_handle[1]);
	destroy_fbo((GLuint *)&state->staging.native_handle[0],
		(GLuint *)&state->staging.native_handle[1]);
	gles2_destroy_blur_levels(state);
	create_fbo(blur_w, blur_h, (GLuint *)&state->ping.native_handle[0],
		(GLuint *)&state->ping.native_handle[1]);
	create_fbo(blur_w, blur_h, (GLuint *)&state->pong.native_handle[0],
		(GLuint *)&state->pong.native_handle[1]);
	create_fbo(blur_w, blur_h, (GLuint *)&state->blur_scratch.native_handle[0],
		(GLuint *)&state->blur_scratch.native_handle[1]);
	state->ping.width = blur_w;
	state->ping.height = blur_h;
	state->pong.width = blur_w;
	state->pong.height = blur_h;
	state->blur_scratch.width = blur_w;
	state->blur_scratch.height = blur_h;

	create_fbo(width, height, (GLuint *)&state->staging.native_handle[0],
		(GLuint *)&state->staging.native_handle[1]);
	state->staging.width = width;
	state->staging.height = height;

	if (!create_fbo(width, height, (GLuint *)&state->screen_shader.native_handle[0],
		(GLuint *)&state->screen_shader.native_handle[1]))
		wlr_log(WLR_ERROR, "gles2: screen shader FBO resize failed (non-fatal)");
	state->screen_shader.width = width;
	state->screen_shader.height = height;

	egl_unset_current();
}

static bool gles2_ensure_buffer(struct wlr_buffer **buf, uint64_t native[2], int w, int h,
		struct wlr_renderer *r, struct wlr_allocator *a) {
	if (*buf)
		return native[0] != 0;
	if (!g->render_fmt)
		return false;

	struct wlr_buffer *new_buf = wlr_allocator_create_buffer(a, w, h, g->render_fmt);
	if (!new_buf)
		return false;

	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(r, new_buf);
	if (!fbo) {
		wlr_buffer_drop(new_buf);
		return false;
	}

	struct wlr_texture *tex = wlr_texture_from_buffer(r, new_buf);
	if (tex) {
		struct wlr_gles2_texture_attribs attribs;
		wlr_gles2_texture_get_attribs(tex, &attribs);
		native[1] = (uint64_t)attribs.tex;
		wlr_texture_destroy(tex);
	}

	wlr_buffer_lock(new_buf);
	wlr_buffer_drop(new_buf);
	*buf = new_buf;
	native[0] = (uint64_t)fbo;
	return true;
}

static void gles2_destroy_buffer(struct wlr_buffer *buf, uint64_t native[2]) {
	if (!buf)
		return;
	wlr_buffer_unlock(buf);
	native[0] = native[1] = 0;
}

static void gles2_frame_begin(void) {
	egl_make_current();
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
}

static void gles2_frame_end(void) {
	glFinish();
	egl_unset_current();
}

static bool gles2_blit(uint64_t src_tex, uint64_t dst_fbo, int w, int h,
		const pixman_box32_t *scissor, int n_scissor) {
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
	glUseProgram(g->prog_blit);
	glUniform1i(g->u_blit.tex, 0);

	if (n_scissor > 0 && scissor) {
		glEnable(GL_SCISSOR_TEST);
		for (int i = 0; i < n_scissor; i++) {
			glScissor(scissor[i].x1, h - scissor[i].y2, scissor[i].x2 - scissor[i].x1,
				scissor[i].y2 - scissor[i].y1);
			draw_quad();
		}
		glDisable(GL_SCISSOR_TEST);
	} else {
		draw_quad();
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_blur(be_output_state_t *state, uint64_t src_handle, int src_w, int src_h,
		struct be_blur_params *p, uint64_t dst_fbo, const pixman_box32_t *scissor, int n_scissor,
		uint64_t *out_handle) {
	GLuint dst = (GLuint)dst_fbo;

	if (p->passes <= 0 || p->algorithm == BLUR_ALGORITHM_NONE) {
		if (dst) {
			gles2_blit(src_handle, dst_fbo, src_w, src_h, NULL, 0);
			if (out_handle)
				*out_handle = 0;
		} else {
			*out_handle = src_handle;
		}
		return true;
	}

	// full-res dual-kawase pyramid
	if (p->full_res && p->algorithm == BLUR_ALGORITHM_KAWASE) {
		int passes = p->passes > BLUR_MAX_LEVELS ? BLUR_MAX_LEVELS : p->passes;
		GLuint level_fbo[BLUR_MAX_LEVELS];
		GLuint level_tex[BLUR_MAX_LEVELS];
		int lw[BLUR_MAX_LEVELS], lh[BLUR_MAX_LEVELS];
		GLuint cur_tex = (GLuint)src_handle;
		int cur_w = src_w, cur_h = src_h;

		for (int i = 0; i < passes; i++) {
			int dw = cur_w > 1 ? cur_w / 2 : 1;
			int dh = cur_h > 1 ? cur_h / 2 : 1;
			GLuint fbo = gles2_ensure_blur_level(state, i, dw, dh);
			if (!fbo)
				return false;
			level_fbo[i] = fbo;
			level_tex[i] = (GLuint)state->blur_levels[i].native_handle[1];
			lw[i] = dw;
			lh[i] = dh;

			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glViewport(0, 0, dw, dh);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, cur_tex);
			glUseProgram(g->prog_blur_down);
			glUniform1i(g->u_blur_down.tex, 0);
			glUniform2f(g->u_blur_down.halfpixel, 0.5f / dw, 0.5f / dh);
			glUniform1f(g->u_blur_down.offset, p->offset);
			draw_quad();

			cur_tex = level_tex[i];
			cur_w = dw;
			cur_h = dh;
		}

		for (int i = passes - 1; i >= 0; i--) {
			bool final = (i == 0);
			int uw = final ? src_w : lw[i - 1];
			int uh = final ? src_h : lh[i - 1];
			GLuint target = final ?
				(dst ? dst : (GLuint)state->blur_scratch.native_handle[0]) : level_fbo[i - 1];

			glBindFramebuffer(GL_FRAMEBUFFER, target);
			glViewport(0, 0, uw, uh);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, cur_tex);
			glUseProgram(g->prog_blur_up);
			glUniform1i(g->u_blur_up.tex, 0);
			glUniform2f(g->u_blur_up.halfpixel, 0.5f / lw[i], 0.5f / lh[i]);
			glUniform1f(g->u_blur_up.offset, p->offset);
			glUniform1f(g->u_blur_up.adjust, final ? 1.0f : 0.0f);
			glUniform1f(g->u_blur_up.saturation, p->saturation);
			glUniform1f(g->u_blur_up.noise_strength, p->noise_strength);
			glUniform1f(g->u_blur_up.vibrancy, p->vibrancy);
			glUniform1f(g->u_blur_up.vibrancy_darkness, p->vibrancy_darkness);
			glUniform1f(g->u_blur_up.brightness, p->brightness);
			glUniform1f(g->u_blur_up.contrast, p->contrast);
			draw_quad();

			if (final)
				cur_tex = dst ? 0 : (GLuint)state->blur_scratch.native_handle[1];
			else
				cur_tex = level_tex[i - 1];
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (out_handle)
			*out_handle = dst ? 0 : (uint64_t)state->blur_scratch.native_handle[1];
		return true;
	}

	int ping = 0;
	GLuint current = (GLuint)src_handle;

	// Keep the captured backdrop in pong so partial damage never mixes with a previous blur.
	GLuint fbo0 = (GLuint)state->ping.native_handle[0];
	GLuint tex0 = (GLuint)state->ping.native_handle[1];
	GLuint fbo1 = (GLuint)state->blur_scratch.native_handle[0];
	GLuint tex1 = (GLuint)state->blur_scratch.native_handle[1];
	if (current == tex0) {
		fbo0 = (GLuint)state->pong.native_handle[0];
		tex0 = (GLuint)state->pong.native_handle[1];
	} else if (current == tex1) {
		fbo1 = (GLuint)state->pong.native_handle[0];
		tex1 = (GLuint)state->pong.native_handle[1];
	}

	for (int i = 0; i < p->passes; i++) {
		bool last = dst && (i == p->passes - 1);
		int pong = ping ^ 1;
		if (p->algorithm == BLUR_ALGORITHM_GAUSSIAN) {
			if (last) {
				gaussian_pass_dir(g->prog_gauss_h, current, fbo0, src_w, src_h, p, scissor, n_scissor);
				gaussian_pass_dir(g->prog_gauss_v, tex0, dst, src_w, src_h, p, scissor, n_scissor);
				current = dst;
			} else {
				gaussian_pass(current, fbo0, tex0, fbo1, src_w, src_h, p, scissor, n_scissor);
				current = tex1;
			}
		} else if (p->algorithm == BLUR_ALGORITHM_BOX) {
			if (last) {
				box_pass_dir(g->prog_box_h, current, fbo0, src_w, src_h, p, scissor, n_scissor);
				box_pass_dir(g->prog_box_v, tex0, dst, src_w, src_h, p, scissor, n_scissor);
				current = dst;
			} else {
				box_pass(current, fbo0, tex0, fbo1, src_w, src_h, p, scissor, n_scissor);
				current = tex1;
			}
		} else if (p->algorithm == BLUR_ALGORITHM_REFRACTION ||
				p->algorithm == BLUR_ALGORITHM_LENS_REFRACTION) {
			int mode = (p->algorithm == BLUR_ALGORITHM_LENS_REFRACTION) ? 1 : 0;
			refraction_pass(current, last ? dst : (pong ? fbo1 : fbo0), src_w, src_h, p, mode, scissor,
				n_scissor);
			current = last ? dst : (pong ? tex1 : tex0);
			ping = pong;
		} else {
			blur_pass(current, last ? dst : (ping ? fbo1 : fbo0), src_w, src_h, i, p, scissor, n_scissor);
			current = last ? dst : (ping ? tex1 : tex0);
			ping ^= 1;
		}
	}

	// Copy the source when no pass produced a separate blur texture.
	if (!dst && current == (GLuint)src_handle) {
		gles2_blit(current, fbo0, src_w, src_h, NULL, 0);
		current = tex0;
	}

	if (out_handle)
		*out_handle = dst ? 0 : (uint64_t)current;
	return true;
}

static bool gles2_apply_mica_tint(be_output_state_t *state, uint64_t bg_handle, float tint[4],
		float tint_strength, uint64_t dst_fbo, int w, int h) {
	(void)state;
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bg_handle);
	glUseProgram(g->prog_mica_tint);
	glUniform1i(g->u_mica.tex, 0);
	glUniform4fv(g->u_mica.tint, 1, tint);
	glUniform1f(g->u_mica.tint_strength, tint_strength);
	draw_quad();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_apply_acrylic(be_output_state_t *state, uint64_t bg_handle,
		struct be_acrylic_params *p, uint64_t dst_fbo, int w, int h) {
	GLuint blurred = (GLuint)bg_handle;
	int blur_w = state->ping.width > 0 ? state->ping.width : w;
	int blur_h = state->ping.height > 0 ? state->ping.height : h;
	if (p->blur_passes > 0) {
		GLuint fbos[2] = {
			(GLuint)state->ping.native_handle[0],
			(GLuint)state->blur_scratch.native_handle[0],
		};
		GLuint texs[2] = {
			(GLuint)state->ping.native_handle[1],
			(GLuint)state->blur_scratch.native_handle[1],
		};
		GLuint current = (GLuint)bg_handle;
		for (int i = 0; i < p->blur_passes; i++) {
			int target = i & 1;
			blur_pass(current, fbos[target], blur_w, blur_h, i, &(struct be_blur_params){
				.radius = p->blur_radius
			}, NULL, 0);
			current = texs[target];
		}
		blurred = current;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, blurred);
	glUseProgram(g->prog_acrylic_tint);
	glUniform1i(g->u_acrylic.tex, 0);
	glUniform4fv(g->u_acrylic.tint, 1, p->tint);
	glUniform1f(g->u_acrylic.tint_strength, p->tint_strength);
	glUniform1f(g->u_acrylic.noise_strength, p->noise_strength);
	glUniform2f(g->u_acrylic.resolution, p->res_w, p->res_h);
	glUniform2f(g->u_acrylic.light_anchor, p->light_anchor_x, p->light_anchor_y);
	draw_quad();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_render_shadow(struct be_shadow_params *p, uint64_t dst_fbo) {
	if (!g->prog_shadow)
		return false;

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, p->buf_w, p->buf_h);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(g->prog_shadow);

	glUniform2f(g->u_shadow.resolution, (float)p->buf_w, (float)p->buf_h);
	glUniform1f(g->u_shadow.shadow_size, p->shadow_size);
	glUniform4fv(g->u_shadow.shadow_color, 1, p->shadow_color);
	glUniform1f(g->u_shadow.border_radius, p->border_radius);
	glUniform2f(g->u_shadow.inner_size, p->inner_width, p->inner_height);
	glUniform2f(g->u_shadow.hole_pos, p->hole_x, p->hole_y);
	glUniform2f(g->u_shadow.hole_size, p->hole_width, p->hole_height);

	draw_quad();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_render_border(struct be_border_params *p, uint64_t dst_fbo) {
	if (!g->prog_border)
		return false;

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, p->buf_w, p->buf_h);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(g->prog_border);

	glUniform2f(g->u_border.resolution, p->res_w, p->res_h);
	glUniform1f(g->u_border.border_radius, p->border_radius);
	glUniform1f(g->u_border.border_width_px, p->border_width_px);
	glUniform1f(g->u_border.scale, p->scale);
	glUniform4fv(g->u_border.border_color, 1, p->border_color);

	if (g->u_border.gradient_colors >= 0)
		glUniform4fv(g->u_border.gradient_colors, 10, p->gradient_colors);
	if (g->u_border.gradient_count >= 0)
		glUniform1i(g->u_border.gradient_count, p->gradient_count);
	if (g->u_border.gradient_angle >= 0)
		glUniform1f(g->u_border.gradient_angle, p->gradient_angle);
	if (g->u_border.gradient2_colors >= 0)
		glUniform4fv(g->u_border.gradient2_colors, 10, p->gradient2_colors);
	if (g->u_border.gradient2_count >= 0)
		glUniform1i(g->u_border.gradient2_count, p->gradient2_count);
	if (g->u_border.gradient2_angle >= 0)
		glUniform1f(g->u_border.gradient2_angle, p->gradient2_angle);
	if (g->u_border.gradient_lerp >= 0)
		glUniform1f(g->u_border.gradient_lerp, p->gradient_lerp);

	draw_quad();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_apply_corner_mask(be_output_state_t *state, uint64_t dst_fbo, int dst_w, int dst_h,
		uint64_t bg_tex, struct be_corner_mask_params *p) {
	(void)state;
	if (!g->prog_corner_mask)
		return false;

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, dst_w, dst_h);

	if (p->pre_blit) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bg_tex);
	glUseProgram(g->prog_corner_mask);
	glUniform1i(g->u_corner_mask.tex, 0);
	glUniform2f(g->u_corner_mask.win_pos_uv, p->win_u, p->win_v);
	glUniform2f(g->u_corner_mask.win_size_uv, p->win_sw, p->win_sh);
	glUniform2f(g->u_corner_mask.win_size_px, p->win_size_px_w, p->win_size_px_h);
	glUniform1f(g->u_corner_mask.border_radius_px, p->border_radius_px);
	glUniform1f(g->u_corner_mask.scale, p->scale);
	glUniform2f(g->u_corner_mask.bg_pos_uv, p->bg_u, p->bg_v);
	glUniform2f(g->u_corner_mask.bg_size_uv, p->bg_sw, p->bg_sh);
	draw_quad();

	if (p->pre_blit)
		glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_apply_screen_shader(uint64_t src_tex, uint64_t dst_fbo, int w, int h,
		struct be_screen_shader_params *p) {
	if (!g->screen_shader_prog)
		return false;

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
	glViewport(0, 0, w, h);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
	glUseProgram(g->screen_shader_prog);

	if (g->screen_shader_u_tex >= 0)
		glUniform1i(g->screen_shader_u_tex, 0);
	if (g->screen_shader_u_resolution >= 0)
		glUniform2f(g->screen_shader_u_resolution, (float)w * p->scale, (float)h * p->scale);
	if (g->screen_shader_u_time >= 0)
		glUniform1f(g->screen_shader_u_time, p->time);

	draw_quad();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

static bool gles2_capture_readback(struct wlr_buffer *capture_buffer, be_output_state_t *state,
		uint64_t dst_fbo, int dst_x, int dst_y, int dst_w, int dst_h, int src_x, int src_y, int src_w,
		int src_h, uint64_t *out_tex) {
	GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(g->renderer, capture_buffer);
	if (!capture_fbo) {
		wlr_log(WLR_INFO, "gles2: capture_readback: no FBO");
		return false;
	}

	GLuint result_tex = 0;

	glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
	GLint attach_type = 0, attach_name = 0;
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attach_type);
	if (attach_type == GL_TEXTURE) {
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attach_name);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (attach_type == GL_TEXTURE && attach_name > 0 && g->prog_ext_blit) {
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
		glViewport(dst_x, dst_y, dst_w, dst_h);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)attach_name);
		glUseProgram(g->prog_ext_blit);
		glUniform1i(g->u_ext_blit.tex, 0);
		draw_quad();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		result_tex = (GLuint)state->pong.native_handle[1];
		if (dst_fbo == state->screen_shader.native_handle[0])
			result_tex = (GLuint)state->screen_shader.native_handle[1];
	} else if (attach_type == GL_TEXTURE && attach_name > 0) {
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
		glViewport(dst_x, dst_y, dst_w, dst_h);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, (GLuint)attach_name);
		glUseProgram(g->prog_blit);
		glUniform1i(g->u_blit.tex, 0);
		draw_quad();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		result_tex = (GLuint)state->pong.native_handle[1];
		if (dst_fbo == state->screen_shader.native_handle[0])
			result_tex = (GLuint)state->screen_shader.native_handle[1];
	} else if (attach_type == GL_RENDERBUFFER) {
		// resize staging texture to match so blit below is a 1:1 copy
		if (state->staging.width != src_w || state->staging.height != src_h) {
			destroy_fbo((GLuint *)&state->staging.native_handle[0],
				(GLuint *)&state->staging.native_handle[1]);
			if (!create_fbo(src_w, src_h, (GLuint *)&state->staging.native_handle[0],
				(GLuint *)&state->staging.native_handle[1]))
				return false;
			state->staging.width = src_w;
			state->staging.height = src_h;
		}

		GLuint staging_tex = (GLuint)state->staging.native_handle[1];
		if (src_w <= 0 || src_h <= 0 || (size_t)src_w > SIZE_MAX / 4 / (size_t)src_h)
			return false;
		size_t buf_size = (size_t)src_w * (size_t)src_h * 4;
		if (buf_size > g->readback_pixels_size) {
			uint8_t *pixels = realloc(g->readback_pixels, buf_size);
			if (!pixels)
				return false;
			g->readback_pixels = pixels;
			g->readback_pixels_size = buf_size;
		}

		// Read through RGBA because NVIDIA rejects copying this renderbuffer into an RGBA texture.
		glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
		glReadPixels(src_x, src_y, src_w, src_h, GL_RGBA, GL_UNSIGNED_BYTE, g->readback_pixels);
		glBindTexture(GL_TEXTURE_2D, staging_tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h, GL_RGBA, GL_UNSIGNED_BYTE,
			g->readback_pixels);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)dst_fbo);
		glViewport(dst_x, dst_y, dst_w, dst_h);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, staging_tex);
		glUseProgram(g->prog_blit);
		glUniform1i(g->u_blit.tex, 0);
		draw_quad();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		result_tex = (GLuint)state->pong.native_handle[1];
		if (dst_fbo == state->screen_shader.native_handle[0])
			result_tex = (GLuint)state->screen_shader.native_handle[1];
	}

	if (!result_tex)
		return false;
	*out_tex = (uint64_t)result_tex;
	return true;
}

static bool gles2_compile_screen_shader(const char *frag_src) {
	if (g->screen_shader_prog)
		glDeleteProgram(g->screen_shader_prog);
	g->screen_shader_prog = 0;

	GLuint prog = link_program(frag_src);
	if (!prog)
		return false;

	g->screen_shader_prog = prog;
	g->screen_shader_u_tex = glGetUniformLocation(prog, "tex");
	g->screen_shader_u_resolution = glGetUniformLocation(prog, "resolution");
	g->screen_shader_u_time = glGetUniformLocation(prog, "time");
	return true;
}

static void gles2_destroy_screen_shader(void) {
	if (g->screen_shader_prog) {
		glDeleteProgram(g->screen_shader_prog);
		g->screen_shader_prog = 0;
	}
	g->screen_shader_u_tex = -1;
	g->screen_shader_u_resolution = -1;
	g->screen_shader_u_time = -1;
}

static const char *gles2_get_screen_shader_source(const char *name) {
	if (strcmp(name, "grayscale") == 0)
		return gl_grayscale_frag_src;
	if (strcmp(name, "invert") == 0)
		return gl_invert_frag_src;
	if (strcmp(name, "sepia") == 0)
		return gl_sepia_frag_src;
	if (strcmp(name, "nightlight") == 0)
		return gl_nightlight_frag_src;
	return NULL;
}

const effects_backend_t gles2_backend = {
	.init = gles2_init,
	.fini = gles2_fini,
	.output_init = gles2_output_init,
	.output_fini = gles2_output_fini,
	.output_resize = gles2_output_resize,
	.ensure_buffer = gles2_ensure_buffer,
	.destroy_buffer = gles2_destroy_buffer,
	.frame_begin = gles2_frame_begin,
	.frame_end = gles2_frame_end,
	.blit = gles2_blit,
	.blur = gles2_blur,
	.apply_mica_tint = gles2_apply_mica_tint,
	.apply_acrylic = gles2_apply_acrylic,
	.render_shadow = gles2_render_shadow,
	.render_border = gles2_render_border,
	.apply_corner_mask = gles2_apply_corner_mask,
	.apply_screen_shader = gles2_apply_screen_shader,
	.capture_readback = gles2_capture_readback,
	.get_screen_shader_source = gles2_get_screen_shader_source,
	.compile_screen_shader = gles2_compile_screen_shader,
	.destroy_screen_shader = gles2_destroy_screen_shader,
};


#endif
