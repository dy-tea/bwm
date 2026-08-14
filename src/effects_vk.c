#include "effects_backend.h"

#include <drm_fourcc.h>
#include <wlr/config.h>
#include <wlr/util/log.h>

#ifdef WLR_HAS_VULKAN_RENDERER

#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/vulkan.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

#include "vk_grayscale_frag_src.h"
#include "vk_invert_frag_src.h"
#include "vk_nightlight_frag_src.h"
#include "vk_sepia_frag_src.h"

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

#include "vk_blit_frag_src.h"
#include "vk_blur_acrylic_frag_src.h"
#include "vk_blur_box_h_frag_src.h"
#include "vk_blur_box_v_frag_src.h"
#include "vk_blur_gauss_h_frag_src.h"
#include "vk_blur_gauss_v_frag_src.h"
#include "vk_blur_kawase_frag_src.h"
#include "vk_blur_down_frag_src.h"
#include "vk_blur_up_frag_src.h"
#include "vk_blur_mica_frag_src.h"
#include "vk_blur_refraction_frag_src.h"
#include "vk_border_corner_mask_frag_src.h"
#include "vk_border_frag_src.h"
#include "vk_effect_tex_vert_src.h"
#include "vk_shadow_frag_src.h"

struct vk_image {
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	int width;
	int height;
};

struct vk_fbo {
	struct vk_image img;
	VkFramebuffer fb;
	struct wlr_texture *tex; // owning reference to the wlr_texture wrapping the buffer
};

union vk_push_data {
	struct {
		float d[32];
	} raw;
	struct {
		float hp[2];
		float off;
		float noise;
		float vib;
		float vd;
		float br;
		float cont;
	} kawase;
	struct {
		float hp[2];
		float off;
		float adjust;
		float sat;
		float vib;
		float vd;
		float br;
		float cont;
		float noise;
	} pyramid;
	struct {
		float texel[2];
		float radius;
		float _pad0;
		float vib;
		float vd;
		float br;
		float cont;
	} gauss;
	struct {
		float tint[4];
		float strength;
	} mica;
	struct {
		float tint[4];
		float strength;
		float noise;
		float res[2];
		float anchor[2];
	} acrylic;
	struct {
		float hp[2];
		float off;
		float rsize[2];
		float edge;
		float corner;
		float str;
		float npow;
		float rf;
		int rm;
		int mode;
		int _pad0;
	} refraction;
	struct {
		float res[2];
		float size;
		float _pad;
		float col[4];
		float br;
		float _pad2;
		float inner[2];
		float hole[2];
		float hsize[2];
	} shadow;
	struct {
		float wpos[2];
		float wsz[2];
		float wpx[2];
		float br;
		float scale;
		int pre;
	} corner;
	struct {
		float res[2];
		float time;
		float _pad[2];
	} screen;
};

struct vk_border_ubo {
	float gradient_colors[40];
	float gradient2_colors[40];
	int gradient_count;
	int gradient2_count;
	float gradient_angle;
	float gradient2_angle;
	float gradient_lerp;
};

struct vk_data {
	VkInstance instance;
	VkPhysicalDevice phys_dev;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;

	VkCommandPool cmd_pool;
	VkCommandBuffer frame_cb;
	VkCommandBuffer frame_cb_bufs[3];
	VkRenderPass render_pass;
	VkRenderPass no_clear_render_pass;
	VkRenderPass overlay_render_pass;
	VkRenderPass color_clear_render_pass;
	VkDescriptorSetLayout ds_layout;
	VkDescriptorSetLayout ds_border_layout;
	VkPipelineLayout pipe_layout;
	VkPipelineLayout border_pipe_layout;
	VkSampler sampler;
	VkDescriptorPool desc_pool;
	VkDescriptorPool desc_pool_bufs[3];

	VkShaderModule vert_module;
	VkShaderModule frag_blit;
	VkShaderModule frag_kawase;
	VkShaderModule frag_blur_down;
	VkShaderModule frag_blur_up;
	VkShaderModule frag_gauss_h, frag_gauss_v;
	VkShaderModule frag_box_h, frag_box_v;
	VkShaderModule frag_mica;
	VkShaderModule frag_acrylic;
	VkShaderModule frag_refraction;
	VkShaderModule frag_shadow;
	VkShaderModule frag_border;
	VkShaderModule frag_corner_mask;

	VkPipeline pipe_blit;
	VkPipeline pipe_kawase;
	VkPipeline pipe_blur_down;
	VkPipeline pipe_blur_up;
	VkPipeline pipe_gauss_h, pipe_gauss_v;
	VkPipeline pipe_box_h, pipe_box_v;
	VkPipeline pipe_mica;
	VkPipeline pipe_acrylic;
	VkPipeline pipe_refraction;
	VkPipeline pipe_shadow;
	VkPipeline pipe_border;
	VkPipeline pipe_corner_mask;
	VkPipeline pipe_corner_mask_clear;
	VkShaderModule screen_shader_module;
	VkPipeline screen_shader_pipe;

	VkBuffer border_ubo;
	VkDeviceMemory border_ubo_mem;
	void *border_ubo_map;
	VkDescriptorPool border_ds_pool;
	VkDescriptorSet border_ds;

	VkImage dummy_img;
	VkDeviceMemory dummy_img_mem;
	VkImageView dummy_view;
	VkDescriptorPool dummy_ds_pool;
	VkDescriptorSet dummy_ds;

	VkFormat vk_fmt;
	const struct wlr_drm_format *render_fmt;
	struct wlr_renderer *wlr_renderer;
	struct wlr_allocator *allocator;
	int blur_w, blur_h;
	uint32_t vendor_id;

	VkFence frame_fence[3];
	int frame_slot;
	bool frame_dirty;
	bool cb_begun;

	VkImageView deferred_views[3][64];
	int n_deferred_views[3];

	struct vk_fbo *pending_fbo_destroys[256];
	int n_pending_fbo_destroys;

#define VK_VIEW_CACHE_SIZE 64
	VkImage cached_images[3][VK_VIEW_CACHE_SIZE];
	VkImageView cached_views[3][VK_VIEW_CACHE_SIZE];
	int n_cached_views[3];

	struct vk_fbo *scratch_fbo;
	int scratch_w, scratch_h;

#define VK_MAX_DRAW_CALLS 256
	VkDescriptorSet desc_sets[3][VK_MAX_DRAW_CALLS];
	int ds_idx[3];
};

static struct vk_data *vk = NULL;

static VkShaderModule vk_compile_shader(const char *glsl_src, VkShaderStageFlagBits stage) {
	shaderc_compiler_t compiler = shaderc_compiler_initialize();
	if (!compiler) {
		wlr_log(WLR_ERROR, "vk: failed to init shaderc");
		return VK_NULL_HANDLE;
	}

	shaderc_shader_kind kind = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? shaderc_glsl_vertex_shader :
		shaderc_glsl_fragment_shader;

	shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, glsl_src, strlen(glsl_src),
		kind, "shader", "main", NULL);

	if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
		wlr_log(WLR_ERROR, "vk: shader error: %s", shaderc_result_get_error_message(result));
		shaderc_result_release(result);
		shaderc_compiler_release(compiler);
		return VK_NULL_HANDLE;
	}

	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shaderc_result_get_length(result),
		.pCode = (const uint32_t *)shaderc_result_get_bytes(result),
	};
	VkShaderModule module;
	if (vkCreateShaderModule(vk->device, &ci, NULL, &module) != VK_SUCCESS)
		module = VK_NULL_HANDLE;
	shaderc_result_release(result);
	shaderc_compiler_release(compiler);
	return module;
}

static VkPipeline vk_create_pipe(VkShaderModule frag_mod, bool use_border_layout,
		bool blend_enable) {
	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vk->vert_module,
			.pName = "main"
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = frag_mod,
			.pName = "main"
		},
	};
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
	};
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};
	VkViewport viewport = {
		0,
		0,
		1,
		1,
		0,
		1
	};
	VkRect2D scissor = {
		{0, 0},
		{1, 1}
	};
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
	};
	VkPipelineColorBlendAttachmentState cb_att = {
		.blendEnable = blend_enable ? VK_TRUE : VK_FALSE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &cb_att,
	};
	VkDynamicState dyn[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dyn_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dyn,
	};
	VkGraphicsPipelineCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vi,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs,
		.pMultisampleState = &ms,
		.pDepthStencilState = &ds,
		.pColorBlendState = &cb,
		.pDynamicState = &dyn_state,
		.layout = use_border_layout ? vk->border_pipe_layout : vk->pipe_layout,
		.renderPass = vk->render_pass,
		.subpass = 0,
	};
	VkPipeline pipe;
	if (vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &ci, NULL, &pipe) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	return pipe;
}

static int vk_find_mem_type(VkPhysicalDevice phys_dev, VkMemoryPropertyFlags flags,
		uint32_t req_bits) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(phys_dev, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((req_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
			return (int)i;
	}
	return -1;
}

static bool vk_create_image(int w, int h, VkFormat fmt, VkImageUsageFlags usage,
		struct vk_image *out) {
	VkImageCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = {w, h, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.format = fmt,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.usage = usage,
		.samples = VK_SAMPLE_COUNT_1_BIT,
	};
	if (vkCreateImage(vk->device, &ci, NULL, &out->image) != VK_SUCCESS)
		return false;

	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(vk->device, out->image, &mr);
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = vk_find_mem_type(vk->phys_dev, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			mr.memoryTypeBits),
	};
	if (vkAllocateMemory(vk->device, &ai, NULL, &out->memory) != VK_SUCCESS) {
		vkDestroyImage(vk->device, out->image, NULL);
		return false;
	}
	vkBindImageMemory(vk->device, out->image, out->memory, 0);

	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = out->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = fmt,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vkCreateImageView(vk->device, &vci, NULL, &out->view) != VK_SUCCESS) {
		vkFreeMemory(vk->device, out->memory, NULL);
		vkDestroyImage(vk->device, out->image, NULL);
		return false;
	}
	out->width = w;
	out->height = h;
	return true;
}

static void vk_transition_to_shader_read(VkImage image) {
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};

	if (vk->cb_begun) {
		vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
		return;
	}

	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb;
	if (vkAllocateCommandBuffers(vk->device, &cai, &cb) != VK_SUCCESS)
		return;
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	vkBeginCommandBuffer(cb, &bi);
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &barrier);
	vkEndCommandBuffer(cb);
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cb
	};
	vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(vk->queue);
	vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cb);
}

static void vk_destroy_image(struct vk_image *img) {
	if (img->view) {
		vkDestroyImageView(vk->device, img->view, NULL);
		img->view = VK_NULL_HANDLE;
	}
	if (img->image) {
		vkDestroyImage(vk->device, img->image, NULL);
		img->image = VK_NULL_HANDLE;
	}
	if (img->memory) {
		vkFreeMemory(vk->device, img->memory, NULL);
		img->memory = VK_NULL_HANDLE;
	}
}

static bool vk_create_fbo(int w, int h, VkFormat fmt, struct vk_fbo *out) {
	if (!vk_create_image(w, h, fmt,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		| VK_IMAGE_USAGE_TRANSFER_DST_BIT, &out->img))
		return false;
	vk_transition_to_shader_read(out->img.image);
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = vk->render_pass,
		.attachmentCount = 1,
		.pAttachments = &out->img.view,
		.width = w,
		.height = h,
		.layers = 1,
	};
	if (vkCreateFramebuffer(vk->device, &fci, NULL, &out->fb) != VK_SUCCESS) {
		vk_destroy_image(&out->img);
		return false;
	}
	return true;
}

static void vk_destroy_fbo(struct vk_fbo *fbo) {
	if (fbo->fb) {
		vkDestroyFramebuffer(vk->device, fbo->fb, NULL);
		fbo->fb = VK_NULL_HANDLE;
	}
	vk_destroy_image(&fbo->img);
}

static void vk_defer_view(VkImageView view) {
	int s = vk->frame_slot;
	if (vk->n_deferred_views[s] < 64)
		vk->deferred_views[s][vk->n_deferred_views[s]++] = view;
	else
		vkDestroyImageView(vk->device, view, NULL);
}

static VkImageView vk_lookup_or_create_view(VkImage image);
static void vk_flush_pending_fbo_destroys(void);

static void vk_ensure_cb_begun(void) {
	if (vk->cb_begun)
		return;
	vk->cb_begun = true;
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(vk->frame_cb, &bi);
}

static void vk_draw_full(VkPipeline pipe, VkImage src_img, VkFramebuffer dst_fb, int w, int h,
		VkImage dst_img, const void *pc_data, size_t pc_size, const VkRect2D *scissor,
		uint32_t n_scissor) {
	vk->frame_dirty = true;
	vk_ensure_cb_begun();
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = dst_img,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

	VkImageView src_view = vk_lookup_or_create_view(src_img);
	if (src_view == VK_NULL_HANDLE)
		return;

	int s = vk->frame_slot;
	if (vk->ds_idx[s] >= VK_MAX_DRAW_CALLS)
		return;
	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk->desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk->ds_layout,
	};
	VkDescriptorSet ds;
	if (vkAllocateDescriptorSets(vk->device, &dsai, &ds) != VK_SUCCESS) {
		wlr_log(WLR_DEBUG, "vk: descriptor pool exhausted, skipping draw call %d/%d",
			vk->ds_idx[s], VK_MAX_DRAW_CALLS);
		return;
	}
	vk->desc_sets[s][vk->ds_idx[s]++] = ds;

	VkDescriptorImageInfo dii = {
		.sampler = vk->sampler,
		.imageView = src_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dii,
	};
	vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

	VkRenderPassBeginInfo rp = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = vk->no_clear_render_pass,
		.framebuffer = dst_fb,
		.renderArea = {
			{0, 0},
			{w, h}
		},
	};
	VkViewport vp = {
		0,
		0,
		(float)w,
		(float)h,
		0,
		1
	};
	VkRect2D full_sc = {
		{0, 0},
		{
			(uint32_t)w,
			(uint32_t)h
		}
	};
	vkCmdBeginRenderPass(vk->frame_cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(vk->frame_cb, 0, 1, &vp);
	vkCmdBindDescriptorSets(vk->frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipe_layout, 0, 1, &ds,
		0, NULL);
	if (pc_data && pc_size)
		vkCmdPushConstants(vk->frame_cb, vk->pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pc_size,
			pc_data);
	vkCmdBindPipeline(vk->frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	if (scissor && n_scissor > 0) {
		for (uint32_t i = 0; i < n_scissor; i++) {
			vkCmdSetScissor(vk->frame_cb, 0, 1, &scissor[i]);
			vkCmdDraw(vk->frame_cb, 4, 1, 0, 0);
		}
	} else {
		vkCmdSetScissor(vk->frame_cb, 0, 1, &full_sc);
		vkCmdDraw(vk->frame_cb, 4, 1, 0, 0);
	}
	vkCmdEndRenderPass(vk->frame_cb);
}

static void vk_draw_full_no_tex(VkPipeline pipe, VkImage dst_img, VkFramebuffer dst_fb, int w, int h,
		bool clear, const void *pc_data, size_t pc_size, VkPipelineLayout layout, VkDescriptorSet ds) {
	vk->frame_dirty = true;
	vk_ensure_cb_begun();
	VkImageMemoryBarrier dst_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = dst_img,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &dst_barrier);

	VkRenderPassBeginInfo rp = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = clear ? vk->color_clear_render_pass : vk->no_clear_render_pass,
		.framebuffer = dst_fb,
		.renderArea = {
			{0, 0},
			{w, h}
		},
		.clearValueCount = clear ? 1 : 0,
		.pClearValues = clear ? &(VkClearValue){
			{
				{0, 0, 0, 0}
			}
		}
 : NULL,
	};
	VkViewport vp = {
		0,
		0,
		(float)w,
		(float)h,
		0,
		1
	};
	VkRect2D sc = {
		{0, 0},
		{w, h}
	};
	vkCmdBeginRenderPass(vk->frame_cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(vk->frame_cb, 0, 1, &vp);
	vkCmdSetScissor(vk->frame_cb, 0, 1, &sc);
	if (ds)
		vkCmdBindDescriptorSets(vk->frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &ds, 0,
			NULL);
	if (pc_data && pc_size)
		vkCmdPushConstants(vk->frame_cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pc_size, pc_data);
	vkCmdBindPipeline(vk->frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(vk->frame_cb, 4, 1, 0, 0);
	vkCmdEndRenderPass(vk->frame_cb);
}

static struct vk_fbo *vk_fbo_of(uint64_t h) {
	return (struct vk_fbo *)(intptr_t)h;
}

static VkImage vk_img_of(uint64_t h) {
	return (VkImage)h;
}

static struct vk_fbo *vk_ensure_blur_level(be_output_state_t *state, int i, int w, int h) {
	if (w <= 0 || h <= 0)
		return NULL;
	be_buffer_t *lv = &state->blur_levels[i];
	if (lv->native_handle[0] && lv->width == w && lv->height == h)
		return vk_fbo_of(lv->native_handle[0]);
	if (lv->native_handle[0]) {
		vk_destroy_fbo(vk_fbo_of(lv->native_handle[0]));
		free(vk_fbo_of(lv->native_handle[0]));
		lv->native_handle[0] = lv->native_handle[1] = 0;
	}
	struct vk_fbo *fbo = calloc(1, sizeof(*fbo));
	if (!fbo || !vk_create_fbo(w, h, vk->vk_fmt, fbo)) {
		free(fbo);
		return NULL;
	}
	lv->native_handle[0] = (uint64_t)(intptr_t)fbo;
	lv->native_handle[1] = (uint64_t)fbo->img.image;
	lv->width = w;
	lv->height = h;
	return fbo;
}

static void vk_destroy_blur_levels(be_output_state_t *state) {
	for (int i = 0; i < BLUR_MAX_LEVELS; i++) {
		if (state->blur_levels[i].native_handle[0]) {
			vk_destroy_fbo(vk_fbo_of(state->blur_levels[i].native_handle[0]));
			free(vk_fbo_of(state->blur_levels[i].native_handle[0]));
		}
	}
	memset(state->blur_levels, 0, sizeof(state->blur_levels));
}

static bool vk_init(struct wlr_renderer *r, struct wlr_allocator *a) {
	vk = calloc(1, sizeof(struct vk_data));
	if (!vk)
		return false;
	vk->wlr_renderer = r;
	vk->allocator = a;

	if (!wlr_renderer_is_vk(r)) {
		wlr_log(WLR_INFO, "vk: renderer is not Vulkan");
		free(vk);
		vk = NULL;
		return false;
	}

	vk->instance = wlr_vk_renderer_get_instance(r);
	vk->phys_dev = wlr_vk_renderer_get_physical_device(r);
	vk->device = wlr_vk_renderer_get_device(r);
	vk->queue_family = wlr_vk_renderer_get_queue_family(r);
	vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vk->phys_dev, &props);
	vk->vendor_id = props.vendorID;
	wlr_log(WLR_INFO, "vk: GPU vendor=0x%x device=0x%x name=%s", props.vendorID, props.deviceID,
		props.deviceName);

	const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(r);
	vk->render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
	if (!vk->render_fmt)
		vk->render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;

	if (!vk->render_fmt) {
		wlr_log(WLR_ERROR, "vk: no render format");
		free(vk);
		vk = NULL;
		return false;
	}

	vk->vk_fmt = VK_FORMAT_B8G8R8A8_UNORM; // DRM_FORMAT_ARGB8888 LE

	VkCommandPoolCreateInfo cpi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = vk->queue_family,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	if (vkCreateCommandPool(vk->device, &cpi, NULL, &vk->cmd_pool) != VK_SUCCESS) {
		free(vk);
		vk = NULL;
		return false;
	}

	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 3,
	};
	if (vkAllocateCommandBuffers(vk->device, &cai, vk->frame_cb_bufs) != VK_SUCCESS) {
		vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
		free(vk);
		vk = NULL;
		return false;
	}
	vk->frame_cb = vk->frame_cb_bufs[0];

	VkAttachmentReference ar = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	};
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &ar
	};
	VkRenderPassCreateInfo rpci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = NULL,
		.subpassCount = 1,
		.pSubpasses = &sp,
	};

	for (int i = 0; i < 4; i++) {
		VkAttachmentLoadOp loadOp = (VkAttachmentLoadOp[]){
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_LOAD_OP_LOAD,
		} [i];
		VkRenderPass *dest = (VkRenderPass * []){
			&vk->render_pass,
			&vk->no_clear_render_pass,
			&vk->color_clear_render_pass,
			&vk->overlay_render_pass,
		} [i];
		VkAttachmentDescription att = {
			.format = vk->vk_fmt,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = loadOp,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		rpci.pAttachments = &att;
		if (vkCreateRenderPass(vk->device, &rpci, NULL, dest) != VK_SUCCESS) {
			vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
			free(vk);
			vk = NULL;
			return false;
		}
	}

	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	vkCreateSampler(vk->device, &sci, NULL, &vk->sampler);

	VkDescriptorSetLayoutBinding bind = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.pImmutableSamplers = &vk->sampler,
	};
	VkDescriptorSetLayoutCreateInfo dsci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &bind,
	};
	if (vkCreateDescriptorSetLayout(vk->device, &dsci, NULL, &vk->ds_layout) != VK_SUCCESS)
		return false;

	VkDescriptorSetLayoutBinding bbind[2] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &vk->sampler},
		{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo bdsci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bbind,
	};
	if (vkCreateDescriptorSetLayout(vk->device, &bdsci, NULL, &vk->ds_border_layout) != VK_SUCCESS)
		return false;

	VkPushConstantRange pcr = {
		VK_SHADER_STAGE_FRAGMENT_BIT,
		0,
		128
	};
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &vk->ds_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr,
	};
	if (vkCreatePipelineLayout(vk->device, &plci, NULL, &vk->pipe_layout) != VK_SUCCESS)
		return false;

	plci.pSetLayouts = &vk->ds_border_layout;
	if (vkCreatePipelineLayout(vk->device, &plci, NULL, &vk->border_pipe_layout) != VK_SUCCESS)
		return false;

	// compile shaders
	vk->vert_module = vk_compile_shader(vk_effect_tex_vert_src, VK_SHADER_STAGE_VERTEX_BIT);

	typedef struct {
		const char *src;
		VkShaderModule *dest;
	} shader_entry_t;
	shader_entry_t frag_shaders[] = {
		{vk_blit_frag_src, &vk->frag_blit},
		{vk_blur_kawase_frag_src, &vk->frag_kawase},
		{vk_blur_down_frag_src, &vk->frag_blur_down},
		{vk_blur_up_frag_src, &vk->frag_blur_up},
		{vk_blur_gauss_h_frag_src, &vk->frag_gauss_h},
		{vk_blur_gauss_v_frag_src, &vk->frag_gauss_v},
		{vk_blur_box_h_frag_src, &vk->frag_box_h},
		{vk_blur_box_v_frag_src, &vk->frag_box_v},
		{vk_blur_mica_frag_src, &vk->frag_mica},
		{vk_blur_acrylic_frag_src, &vk->frag_acrylic},
		{vk_blur_refraction_frag_src, &vk->frag_refraction},
		{vk_shadow_frag_src, &vk->frag_shadow},
		{vk_border_frag_src, &vk->frag_border},
		{vk_border_corner_mask_frag_src, &vk->frag_corner_mask},
	};
	for (size_t i = 0; i < sizeof(frag_shaders) / sizeof(frag_shaders[0]); i++)
		*frag_shaders[i].dest = vk_compile_shader(frag_shaders[i].src, VK_SHADER_STAGE_FRAGMENT_BIT);

	if (!vk->vert_module || !vk->frag_blit || !vk->frag_kawase || !vk->frag_corner_mask) {
		wlr_log(WLR_ERROR, "vk: shaders failed to compile");
		return false;
	}

	typedef struct {
		VkShaderModule frag;
		VkPipeline *dest;
		bool border_layout;
		bool corner_mask;
	} pipe_entry_t;
	pipe_entry_t pipelines[] = {
		{vk->frag_blit, &vk->pipe_blit, false, false},
		{vk->frag_kawase, &vk->pipe_kawase, false, false},
		{vk->frag_blur_down, &vk->pipe_blur_down, false, false},
		{vk->frag_blur_up, &vk->pipe_blur_up, false, false},
		{vk->frag_gauss_h, &vk->pipe_gauss_h, false, false},
		{vk->frag_gauss_v, &vk->pipe_gauss_v, false, false},
		{vk->frag_box_h, &vk->pipe_box_h, false, false},
		{vk->frag_box_v, &vk->pipe_box_v, false, false},
		{vk->frag_mica, &vk->pipe_mica, false, false},
		{vk->frag_acrylic, &vk->pipe_acrylic, false, false},
		{vk->frag_refraction, &vk->pipe_refraction, false, false},
		{vk->frag_shadow, &vk->pipe_shadow, false, false},
		{vk->frag_border, &vk->pipe_border, true, false},
		{vk->frag_corner_mask, &vk->pipe_corner_mask, false, true},
		{vk->frag_corner_mask, &vk->pipe_corner_mask_clear, false, false},
	};
	for (size_t i = 0; i < sizeof(pipelines) / sizeof(pipelines[0]); i++)
		*pipelines[i].dest = vk_create_pipe(pipelines[i].frag, pipelines[i].border_layout,
			pipelines[i].corner_mask);

	if (!vk->pipe_blit || !vk->pipe_kawase || !vk->pipe_blur_down || !vk->pipe_blur_up ||
			!vk->pipe_corner_mask || !vk->pipe_corner_mask_clear) {
		wlr_log(WLR_ERROR, "vk: pipelines failed to create");
		return false;
	}

	// border UBO
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = sizeof(struct vk_border_ubo),
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	};
	if (vkCreateBuffer(vk->device, &bci, NULL, &vk->border_ubo) != VK_SUCCESS)
		return false;
	VkMemoryRequirements bmr;
	vkGetBufferMemoryRequirements(vk->device, vk->border_ubo, &bmr);
	VkMemoryAllocateInfo bai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = bmr.size,
		.memoryTypeIndex = vk_find_mem_type(vk->phys_dev,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bmr.memoryTypeBits),
	};
	if (vkAllocateMemory(vk->device, &bai, NULL, &vk->border_ubo_mem) != VK_SUCCESS) {
		vkDestroyBuffer(vk->device, vk->border_ubo, NULL);
		return false;
	}
	vkBindBufferMemory(vk->device, vk->border_ubo, vk->border_ubo_mem, 0);

	vkMapMemory(vk->device, vk->border_ubo_mem, 0, VK_WHOLE_SIZE, 0, &vk->border_ubo_map);

	VkDescriptorPoolSize bp_sz[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
	};
	VkDescriptorPoolCreateInfo bpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 2,
		.pPoolSizes = bp_sz,
	};
	if (vkCreateDescriptorPool(vk->device, &bpci, NULL, &vk->border_ds_pool) != VK_SUCCESS)
		return false;
	VkDescriptorSetAllocateInfo bdsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk->border_ds_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk->ds_border_layout,
	};
	if (vkAllocateDescriptorSets(vk->device, &bdsai, &vk->border_ds) != VK_SUCCESS)
		return false;
	VkDescriptorBufferInfo bdbi = {
		.buffer = vk->border_ubo,
		.offset = 0,
		.range = sizeof(struct vk_border_ubo)
	};
	VkWriteDescriptorSet bw = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = vk->border_ds,
		.dstBinding = 1,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &bdbi,
	};
	vkUpdateDescriptorSets(vk->device, 1, &bw, 0, NULL);

	VkDescriptorPoolSize ps = {
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_MAX_DRAW_CALLS + 64
	};
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = VK_MAX_DRAW_CALLS + 64,
		.poolSizeCount = 1,
		.pPoolSizes = &ps
	};
	for (int i = 0; i < 3; i++) {
		if (vkCreateDescriptorPool(vk->device, &dpci, NULL, &vk->desc_pool_bufs[i]) != VK_SUCCESS) {
			for (int j = 0; j < i; j++)
				vkDestroyDescriptorPool(vk->device, vk->desc_pool_bufs[j], NULL);
			return false;
		}
	}
	vk->desc_pool = vk->desc_pool_bufs[0];

	// dummy 1x1 white image for placeholder sampler bindings
	VkImageCreateInfo dici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = {1, 1, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.format = vk->vk_fmt,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
	};
	if (vkCreateImage(vk->device, &dici, NULL, &vk->dummy_img) != VK_SUCCESS)
		return false;
	VkMemoryRequirements dmr;
	vkGetImageMemoryRequirements(vk->device, vk->dummy_img, &dmr);
	VkMemoryAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = dmr.size,
		.memoryTypeIndex = vk_find_mem_type(vk->phys_dev, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			dmr.memoryTypeBits),
	};
	if (vkAllocateMemory(vk->device, &dai, NULL, &vk->dummy_img_mem) != VK_SUCCESS)
		return false;
	vkBindImageMemory(vk->device, vk->dummy_img, vk->dummy_img_mem, 0);

	// initialize dummy image to white via staging buffer
	uint32_t white = 0xFFFFFFFFu;
	VkBufferCreateInfo sbci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkBuffer staging;
	VkDeviceMemory staging_mem;
	if (vkCreateBuffer(vk->device, &sbci, NULL, &staging) != VK_SUCCESS)
		return false;
	VkMemoryRequirements smr;
	vkGetBufferMemoryRequirements(vk->device, staging, &smr);
	int smti = vk_find_mem_type(vk->phys_dev,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, smr.memoryTypeBits);
	VkMemoryAllocateInfo smai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = smr.size,
		.memoryTypeIndex = smti >= 0 ? (uint32_t)smti : 0,
	};
	if (vkAllocateMemory(vk->device, &smai, NULL, &staging_mem) != VK_SUCCESS) {
		vkDestroyBuffer(vk->device, staging, NULL);
		return false;
	}
	vkBindBufferMemory(vk->device, staging, staging_mem, 0);
	void *map;
	vkMapMemory(vk->device, staging_mem, 0, 4, 0, &map);
	memcpy(map, &white, 4);
	vkUnmapMemory(vk->device, staging_mem);

	VkCommandBufferAllocateInfo tcai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer tcb;
	vkAllocateCommandBuffers(vk->device, &tcai, &tcb);
	VkCommandBufferBeginInfo tbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(tcb, &tbi);
	VkImageMemoryBarrier imb = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = vk->dummy_img,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
		NULL, 0, NULL, 1, &imb);
	VkBufferImageCopy bic = {
		.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.imageExtent = {1, 1, 1},
	};
	vkCmdCopyBufferToImage(tcb, staging, vk->dummy_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	VkImageMemoryBarrier imb2 = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = vk->dummy_img,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		0, NULL, 0, NULL, 1, &imb2);
	vkEndCommandBuffer(tcb);
	VkSubmitInfo tsi = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &tcb,
	};
	vkQueueSubmit(vk->queue, 1, &tsi, VK_NULL_HANDLE);
	vkQueueWaitIdle(vk->queue);
	vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &tcb);
	vkDestroyBuffer(vk->device, staging, NULL);
	vkFreeMemory(vk->device, staging_mem, NULL);

	VkImageViewCreateInfo dvi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk->dummy_img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = vk->vk_fmt,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vkCreateImageView(vk->device, &dvi, NULL, &vk->dummy_view) != VK_SUCCESS)
		return false;

	// write dummy sampler to border descriptor set binding 0
	VkDescriptorImageInfo dummy_dii = {
		.sampler = vk->sampler,
		.imageView = vk->dummy_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write_dummy = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = vk->border_ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dummy_dii,
	};
	vkUpdateDescriptorSets(vk->device, 1, &write_dummy, 0, NULL);

	// dummy descriptor set for vk->pipe_layout (used by shadow)
	VkDescriptorPoolSize ddps = {
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		1
	};
	VkDescriptorPoolCreateInfo ddpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &ddps
	};
	if (vkCreateDescriptorPool(vk->device, &ddpci, NULL, &vk->dummy_ds_pool) != VK_SUCCESS)
		return false;
	VkDescriptorSetAllocateInfo ddsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk->dummy_ds_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk->ds_layout,
	};
	if (vkAllocateDescriptorSets(vk->device, &ddsai, &vk->dummy_ds) != VK_SUCCESS)
		return false;
	VkWriteDescriptorSet write_dummy2 = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = vk->dummy_ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dummy_dii,
	};
	vkUpdateDescriptorSets(vk->device, 1, &write_dummy2, 0, NULL);

	VkFenceCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT
	};
	for (int i = 0; i < 3; i++) {
		if (vkCreateFence(vk->device, &fci, NULL, &vk->frame_fence[i]) != VK_SUCCESS) {
			wlr_log(WLR_ERROR, "vk: failed to create frame fence %d", i);
			return false;
		}
	}

	vk->frame_slot = 0;
	for (int i = 0; i < 3; i++) {
		vk->n_deferred_views[i] = 0;
		vk->n_cached_views[i] = 0;
	}

	wlr_log(WLR_INFO, "vk: initialised");
	return true;
}

static void vk_fini(void) {
	if (!vk)
		return;
	if (vk->cb_begun)
		vkEndCommandBuffer(vk->frame_cb);
	vkQueueWaitIdle(vk->queue);
	vk_flush_pending_fbo_destroys();

	if (vk->screen_shader_pipe) {
		vkDestroyPipeline(vk->device, vk->screen_shader_pipe, NULL);
	}
	if (vk->screen_shader_module) {
		vkDestroyShaderModule(vk->device, vk->screen_shader_module, NULL);
	}

	VkPipeline *destroy_pipes[] = {
		&vk->pipe_blit,
		&vk->pipe_kawase,
		&vk->pipe_blur_down,
		&vk->pipe_blur_up,
		&vk->pipe_gauss_h,
		&vk->pipe_gauss_v,
		&vk->pipe_box_h,
		&vk->pipe_box_v,
		&vk->pipe_mica,
		&vk->pipe_acrylic,
		&vk->pipe_refraction,
		&vk->pipe_shadow,
		&vk->pipe_border,
		&vk->pipe_corner_mask,
		&vk->pipe_corner_mask_clear,
	};
	VkShaderModule *destroy_mods[] = {
		&vk->vert_module,
		&vk->frag_blit,
		&vk->frag_kawase,
		&vk->frag_blur_down,
		&vk->frag_blur_up,
		&vk->frag_gauss_h,
		&vk->frag_gauss_v,
		&vk->frag_box_h,
		&vk->frag_box_v,
		&vk->frag_mica,
		&vk->frag_acrylic,
		&vk->frag_refraction,
		&vk->frag_shadow,
		&vk->frag_border,
		&vk->frag_corner_mask,
	};
	for (size_t i = 0; i < sizeof(destroy_pipes) / sizeof(destroy_pipes[0]); i++) {
		if (*destroy_pipes[i]) {
			vkDestroyPipeline(vk->device, *destroy_pipes[i], NULL);
			*destroy_pipes[i] = VK_NULL_HANDLE;
		}
	}
	for (size_t i = 0; i < sizeof(destroy_mods) / sizeof(destroy_mods[0]); i++) {
		if (*destroy_mods[i]) {
			vkDestroyShaderModule(vk->device, *destroy_mods[i], NULL);
			*destroy_mods[i] = VK_NULL_HANDLE;
		}
	}

	if (vk->dummy_view)
		vkDestroyImageView(vk->device, vk->dummy_view, NULL);
	if (vk->dummy_img_mem)
		vkFreeMemory(vk->device, vk->dummy_img_mem, NULL);
	if (vk->dummy_img)
		vkDestroyImage(vk->device, vk->dummy_img, NULL);
	if (vk->dummy_ds_pool)
		vkDestroyDescriptorPool(vk->device, vk->dummy_ds_pool, NULL);
	if (vk->border_ds_pool)
		vkDestroyDescriptorPool(vk->device, vk->border_ds_pool, NULL);
	if (vk->border_ubo_mem)
		vkFreeMemory(vk->device, vk->border_ubo_mem, NULL);
	if (vk->border_ubo)
		vkDestroyBuffer(vk->device, vk->border_ubo, NULL);
	for (int i = 0; i < 3; i++)
		if (vk->desc_pool_bufs[i])
			vkDestroyDescriptorPool(vk->device, vk->desc_pool_bufs[i], NULL);
	if (vk->sampler)
		vkDestroySampler(vk->device, vk->sampler, NULL);
	if (vk->border_pipe_layout)
		vkDestroyPipelineLayout(vk->device, vk->border_pipe_layout, NULL);
	if (vk->pipe_layout)
		vkDestroyPipelineLayout(vk->device, vk->pipe_layout, NULL);
	if (vk->ds_border_layout)
		vkDestroyDescriptorSetLayout(vk->device, vk->ds_border_layout, NULL);
	if (vk->ds_layout)
		vkDestroyDescriptorSetLayout(vk->device, vk->ds_layout, NULL);
	if (vk->render_pass)
		vkDestroyRenderPass(vk->device, vk->render_pass, NULL);
	if (vk->no_clear_render_pass)
		vkDestroyRenderPass(vk->device, vk->no_clear_render_pass, NULL);
	if (vk->color_clear_render_pass)
		vkDestroyRenderPass(vk->device, vk->color_clear_render_pass, NULL);
	if (vk->overlay_render_pass)
		vkDestroyRenderPass(vk->device, vk->overlay_render_pass, NULL);
	for (int i = 0; i < 3; i++) {
		vkWaitForFences(vk->device, 1, &vk->frame_fence[i], VK_TRUE, UINT64_MAX);
		vkDestroyFence(vk->device, vk->frame_fence[i], NULL);
		for (int j = 0; j < vk->n_deferred_views[i]; j++)
			vkDestroyImageView(vk->device, vk->deferred_views[i][j], NULL);
		for (int j = 0; j < vk->n_cached_views[i]; j++)
			vkDestroyImageView(vk->device, vk->cached_views[i][j], NULL);
	}
	if (vk->scratch_fbo) {
		vk_destroy_fbo(vk->scratch_fbo);
		free(vk->scratch_fbo);
	}
	if (vk->cmd_pool)
		vkDestroyCommandPool(vk->device, vk->cmd_pool, NULL);
	free(vk);
	vk = NULL;
}

static bool vk_output_init(be_output_state_t *state, int width, int height, int blur_w,
		int blur_h) {
	struct vk_fbo *ping = calloc(1, sizeof(*ping));
	struct vk_fbo *pong = calloc(1, sizeof(*pong));
	if (!ping || !pong) {
		free(ping);
		free(pong);
		return false;
	}
	if (!vk_create_fbo(blur_w, blur_h, vk->vk_fmt, ping) || !vk_create_fbo(blur_w, blur_h, vk->vk_fmt,
			pong)) {
		vk_destroy_fbo(ping);
		free(ping);
		vk_destroy_fbo(pong);
		free(pong);
		return false;
	}
	state->ping.native_handle[0] = (uint64_t)(intptr_t)ping;
	state->ping.native_handle[1] = (uint64_t)ping->img.image;
	state->ping.width = blur_w;
	state->ping.height = blur_h;
	state->pong.native_handle[0] = (uint64_t)(intptr_t)pong;
	state->pong.native_handle[1] = (uint64_t)pong->img.image;
	state->pong.width = blur_w;
	state->pong.height = blur_h;
	vk->blur_w = blur_w;
	vk->blur_h = blur_h;

	struct vk_image *staging = calloc(1, sizeof(struct vk_image));
	if (!staging || !vk_create_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, staging)) {
		free(staging);
		return false;
	}
	vk_transition_to_shader_read(staging->image);
	state->staging.native_handle[0] = 0;
	state->staging.native_handle[1] = (uint64_t)(intptr_t)staging;
	state->staging.width = width;
	state->staging.height = height;

	struct vk_fbo *ss = calloc(1, sizeof(*ss));
	if (ss && vk_create_fbo(width, height, vk->vk_fmt, ss)) {
		state->screen_shader.native_handle[0] = (uint64_t)(intptr_t)ss;
		state->screen_shader.native_handle[1] = (uint64_t)ss->img.image;
		state->screen_shader.width = width;
		state->screen_shader.height = height;
	} else {
		free(ss);
		wlr_log(WLR_ERROR, "vk: screen shader FBO failed (non-fatal)");
	}
	return true;
}

static void vk_output_fini(be_output_state_t *state) {
	if (!vk)
		return;
	vkQueueWaitIdle(vk->queue);
	struct vk_fbo *ping = vk_fbo_of(state->ping.native_handle[0]);
	struct vk_fbo *pong = vk_fbo_of(state->pong.native_handle[0]);
	struct vk_fbo *ss = vk_fbo_of(state->screen_shader.native_handle[0]);
	struct vk_image *staging = (struct vk_image *)(intptr_t)state->staging.native_handle[1];
	if (ping) {
		vk_destroy_fbo(ping);
		free(ping);
	}
	if (pong) {
		vk_destroy_fbo(pong);
		free(pong);
	}
	if (ss) {
		vk_destroy_fbo(ss);
		free(ss);
	}
	if (staging) {
		vk_destroy_image(staging);
		free(staging);
	}
	vk_destroy_blur_levels(state);
	memset(&state->ping, 0, sizeof(state->ping));
	memset(&state->pong, 0, sizeof(state->pong));
	memset(&state->staging, 0, sizeof(state->staging));
	memset(&state->screen_shader, 0, sizeof(state->screen_shader));
}

static void vk_output_resize(be_output_state_t *state, int width, int height, int blur_w,
		int blur_h) {
	vk_output_fini(state);
	vk_output_init(state, width, height, blur_w, blur_h);
}

static bool vk_ensure_buffer(struct wlr_buffer **buf, uint64_t native[2], int w, int h,
		struct wlr_renderer *r, struct wlr_allocator *a) {
	if (*buf)
		return native[0] != 0;
	if (!vk->render_fmt)
		return false;
	struct wlr_buffer *new_buf = wlr_allocator_create_buffer(a, w, h, vk->render_fmt);
	if (!new_buf)
		return false;

	struct wlr_texture *tex = wlr_texture_from_buffer(r, new_buf);
	if (!tex) {
		wlr_buffer_drop(new_buf);
		return false;
	}

	struct wlr_vk_image_attribs vk_attribs;
	wlr_vk_texture_get_image_attribs(tex, &vk_attribs);

	struct vk_fbo *fbo = calloc(1, sizeof(struct vk_fbo));
	if (!fbo) {
		wlr_texture_destroy(tex);
		wlr_buffer_drop(new_buf);
		return false;
	}

	fbo->img.image = vk_attribs.image;
	fbo->img.memory = VK_NULL_HANDLE;
	fbo->img.width = w;
	fbo->img.height = h;
	fbo->tex = tex;

	vk_transition_to_shader_read(vk_attribs.image);

	VkFormat actual_fmt = vk_attribs.format;
	if (actual_fmt == VK_FORMAT_UNDEFINED)
		actual_fmt = vk->vk_fmt;
	if (actual_fmt != vk->vk_fmt)
		wlr_log(WLR_DEBUG, "vk: ensure_buffer format mismatch: render_pass=%d actual=%d", vk->vk_fmt,
			actual_fmt);

	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = vk_attribs.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = actual_fmt,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	if (vkCreateImageView(vk->device, &vci, NULL, &fbo->img.view) != VK_SUCCESS) {
		wlr_texture_destroy(tex);
		free(fbo);
		wlr_buffer_drop(new_buf);
		return false;
	}

	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = vk->render_pass,
		.attachmentCount = 1,
		.pAttachments = &fbo->img.view,
		.width = w,
		.height = h,
		.layers = 1,
	};
	if (vkCreateFramebuffer(vk->device, &fci, NULL, &fbo->fb) != VK_SUCCESS) {
		wlr_log(WLR_INFO, "vk: ensure_buffer framebuffer creation failed (format=%d w=%d h=%d)",
			actual_fmt, w, h);
		vkDestroyImageView(vk->device, fbo->img.view, NULL);
		wlr_texture_destroy(tex);
		free(fbo);
		wlr_buffer_drop(new_buf);
		return false;
	}

	wlr_buffer_lock(new_buf);
	wlr_buffer_drop(new_buf);
	*buf = new_buf;
	native[0] = (uint64_t)(intptr_t)fbo;
	native[1] = (uint64_t)fbo->img.image;
	return true;
}

static void vk_free_fbo_resources(struct vk_fbo *fbo) {
	if (fbo->img.view) {
		vkDestroyImageView(vk->device, fbo->img.view, NULL);
		fbo->img.view = VK_NULL_HANDLE;
	}
	if (fbo->fb) {
		vkDestroyFramebuffer(vk->device, fbo->fb, NULL);
		fbo->fb = VK_NULL_HANDLE;
	}
	if (fbo->tex) {
		wlr_texture_destroy(fbo->tex);
		fbo->tex = NULL;
	}
	free(fbo);
}

static void vk_destroy_buffer(struct wlr_buffer *buf, uint64_t native[2]) {
	if (!buf)
		return;
	struct vk_fbo *fbo = vk_fbo_of(native[0]);
	if (fbo) {
		if (vk->cb_begun) {
			if (vk->n_pending_fbo_destroys < 256)
				vk->pending_fbo_destroys[vk->n_pending_fbo_destroys++] = fbo;
			else
				vk_free_fbo_resources(fbo);
		} else {
			vkQueueWaitIdle(vk->queue);
			vk_free_fbo_resources(fbo);
		}
	}
	wlr_buffer_unlock(buf);
	native[0] = native[1] = 0;
}

static void vk_flush_pending_fbo_destroys(void) {
	if (vk->n_pending_fbo_destroys == 0)
		return;
	vkQueueWaitIdle(vk->queue);
	for (int i = 0; i < vk->n_pending_fbo_destroys; i++)
		vk_free_fbo_resources(vk->pending_fbo_destroys[i]);
	vk->n_pending_fbo_destroys = 0;
}

static void vk_frame_begin(void) {
	if (!vk)
		return;

	vk->frame_slot = (vk->frame_slot + 1) % 3;
	int s = vk->frame_slot;

	vkWaitForFences(vk->device, 1, &vk->frame_fence[s], VK_TRUE, UINT64_MAX);

	for (int i = 0; i < vk->n_cached_views[s]; i++)
		vkDestroyImageView(vk->device, vk->cached_views[s][i], NULL);
	vk->n_cached_views[s] = 0;

	for (int i = 0; i < vk->n_deferred_views[s]; i++)
		vkDestroyImageView(vk->device, vk->deferred_views[s][i], NULL);
	vk->n_deferred_views[s] = 0;

	vk->frame_cb = vk->frame_cb_bufs[s];
	vk->desc_pool = vk->desc_pool_bufs[s];
	vk->frame_dirty = false;
	vk->cb_begun = false;

	vkResetDescriptorPool(vk->device, vk->desc_pool, 0);
	vk->ds_idx[s] = 0;
}

static void vk_frame_end(void) {
	if (!vk)
		return;
	int s = vk->frame_slot;

	if (!vk->cb_begun)
		return;

	vkEndCommandBuffer(vk->frame_cb);

	if (!vk->frame_dirty) {
		vkResetCommandBuffer(vk->frame_cb, 0);
		vk_flush_pending_fbo_destroys();
		return;
	}

	vkResetFences(vk->device, 1, &vk->frame_fence[s]);

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &vk->frame_cb,
	};
	vkQueueSubmit(vk->queue, 1, &si, vk->frame_fence[s]);

	vk_flush_pending_fbo_destroys();
}

static bool vk_blit(uint64_t src_tex, uint64_t dst_fbo, int w, int h, const pixman_box32_t *scissor,
		int n_scissor) {
	vk->frame_dirty = true;
	vk_ensure_cb_begun();
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;

	VkImage src_img = vk_img_of(src_tex);
	int n_regions = (scissor && n_scissor > 0) ? n_scissor : 1;
	float sx = (float)vk->blur_w / (float)w;
	float sy = (vk->blur_h > 0) ? (float)vk->blur_h / (float)h : sx;

	VkImageMemoryBarrier pre_barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.image = src_img,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.image = dst->img.image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
		},
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, pre_barriers);

	for (int i = 0; i < n_regions; i++) {
		int x1 = scissor ? scissor[i].x1 : 0;
		int y1 = scissor ? scissor[i].y1 : 0;
		int x2 = scissor ? scissor[i].x2 : w;
		int y2 = scissor ? scissor[i].y2 : h;
		VkImageBlit region = {
			.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			.srcOffsets = {
				{
					(int32_t)(x1 * sx),
					(int32_t)(y1 * sy),
					0
				},
				{
					(int32_t)(x2 * sx),
					(int32_t)(y2 * sy),
					1
				}
			},
			.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			.dstOffsets = {
				{x1, y1, 0},
				{x2, y2, 1}
			},
		};
		vkCmdBlitImage(vk->frame_cb, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
	}

	VkImageMemoryBarrier post_barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.image = src_img,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.image = dst->img.image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
		},
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, post_barriers);

	return true;
}

static void vk_transition_to_shader_read_after_draw(VkImage image) {
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static int vk_scissor_boxes(const pixman_box32_t *scissor, int n_scissor, int w, int h,
		VkRect2D *out, int max) {
	int n = 0;
	for (int i = 0; i < n_scissor && n < max; i++) {
		int x1 = scissor[i].x1;
		int y1 = scissor[i].y1;
		int x2 = scissor[i].x2;
		int y2 = scissor[i].y2;
		if (x1 < 0)
			x1 = 0;
		if (y1 < 0)
			y1 = 0;
		if (x2 > w)
			x2 = w;
		if (y2 > h)
			y2 = h;
		if (x2 <= x1 || y2 <= y1)
			continue;
		out[n].offset.x = x1;
		out[n].offset.y = y1;
		out[n].extent.width = (uint32_t)(x2 - x1);
		out[n].extent.height = (uint32_t)(y2 - y1);
		n++;
	}
	return n;
}

static bool vk_blur(be_output_state_t *state, uint64_t src_handle, int src_w, int src_h,
		struct be_blur_params *p, uint64_t dst_fbo, const pixman_box32_t *scissor, int n_scissor,
		uint64_t *out_handle) {
	if (p->passes <= 0 || p->algorithm == BLUR_ALGORITHM_NONE) {
		if (dst_fbo) {
			vk_blit(src_handle, dst_fbo, src_w, src_h, NULL, 0);
			if (out_handle)
				*out_handle = 0;
		} else {
			*out_handle = src_handle;
		}
		return true;
	}

	VkImage current = vk_img_of(src_handle);
	struct vk_fbo *fbo0 = vk_fbo_of(state->ping.native_handle[0]);
	struct vk_fbo *fbo1 = vk_fbo_of(state->pong.native_handle[0]);
	VkImage tex0 = vk_img_of(state->ping.native_handle[1]);
	VkImage tex1 = vk_img_of(state->pong.native_handle[1]);
	struct vk_fbo *dst = dst_fbo ? vk_fbo_of(dst_fbo) : NULL;
	VkImage dst_img = dst ? dst->img.image : VK_NULL_HANDLE;

	// full-res dual-kawase pyramid
	if (p->full_res && p->algorithm == BLUR_ALGORITHM_KAWASE) {
		int passes = p->passes > BLUR_MAX_LEVELS ? BLUR_MAX_LEVELS : p->passes;
		VkImage level_img[BLUR_MAX_LEVELS];
		int lw[BLUR_MAX_LEVELS], lh[BLUR_MAX_LEVELS];
		VkImage cur_img = vk_img_of(src_handle);
		int cur_w = src_w, cur_h = src_h;

		for (int i = 0; i < passes; i++) {
			int dw = cur_w > 1 ? cur_w / 2 : 1;
			int dh = cur_h > 1 ? cur_h / 2 : 1;
			struct vk_fbo *lfbo = vk_ensure_blur_level(state, i, dw, dh);
			if (!lfbo)
				return false;
			level_img[i] = lfbo->img.image;
			lw[i] = dw;
			lh[i] = dh;

			union vk_push_data pc = {
				{
					{0}
				}
			};
			pc.pyramid.hp[0] = 0.5f / dw;
			pc.pyramid.hp[1] = 0.5f / dh;
			pc.pyramid.off = p->offset;
			vk_draw_full(vk->pipe_blur_down, cur_img, lfbo->fb, dw, dh, lfbo->img.image, &pc, 128, NULL, 0);
			cur_img = level_img[i];
			cur_w = dw;
			cur_h = dh;
		}

		struct vk_fbo *final_fbo = dst;
		VkImage final_img = dst_img;
		if (!final_fbo)
			final_fbo = fbo0;
		if (!final_img)
			final_img = tex0;
		for (int i = passes - 1; i >= 0; i--) {
			bool final = (i == 0);
			int uw = final ? src_w : lw[i - 1];
			int uh = final ? src_h : lh[i - 1];
			struct vk_fbo *dfbo = final ? final_fbo : vk_ensure_blur_level(state, i - 1, uw, uh);
			if (!dfbo)
				return false;
			VkImage dimg = dfbo->img.image;

			union vk_push_data pc = {
				{
					{0}
				}
			};
			pc.pyramid.hp[0] = 0.5f / lw[i];
			pc.pyramid.hp[1] = 0.5f / lh[i];
			pc.pyramid.off = p->offset;
			pc.pyramid.adjust = final ? 1.0f : 0.0f;
			pc.pyramid.sat = p->saturation;
			pc.pyramid.vib = p->vibrancy;
			pc.pyramid.vd = p->vibrancy_darkness;
			pc.pyramid.br = p->brightness;
			pc.pyramid.cont = p->contrast;
			pc.pyramid.noise = p->noise_strength;
			vk_draw_full(vk->pipe_blur_up, cur_img, dfbo->fb, uw, uh, dimg, &pc, 128, NULL, 0);
			cur_img = dimg;
		}

		if (dst_fbo) {
			vk_transition_to_shader_read_after_draw(dst_img);
			if (out_handle)
				*out_handle = 0;
		} else {
			*out_handle = (uint64_t)tex0;
		}
		return true;
	}

	VkRect2D sc[64];
	int n_sc = (scissor && n_scissor > 0) ? vk_scissor_boxes(scissor, n_scissor, src_w, src_h, sc,
		64) : 0;
	VkRect2D *scp = n_sc ? sc : NULL;

	for (int i = 0; i < p->passes; i++) {
		bool last = dst && (i == p->passes - 1);
		if (p->algorithm == BLUR_ALGORITHM_GAUSSIAN) {
			union vk_push_data pc = {
				{
					{0}
				}
			};
			pc.gauss.texel[0] = 1.0f / src_w;
			pc.gauss.texel[1] = 1.0f / src_h;
			pc.gauss.radius = p->radius;
			pc.gauss._pad0 = 0;
			pc.gauss.vib = p->vibrancy;
			pc.gauss.vd = p->vibrancy_darkness;
			pc.gauss.br = p->brightness;
			pc.gauss.cont = p->contrast;
			vk_draw_full(vk->pipe_gauss_h, current, fbo0->fb, src_w, src_h, fbo0->img.image, &pc, 128, scp,
				n_sc);
			if (last) {
				vk_draw_full(vk->pipe_gauss_v, tex0, dst->fb, src_w, src_h, dst_img, &pc, 128, scp, n_sc);
				current = dst_img;
			} else {
				vk_draw_full(vk->pipe_gauss_v, tex0, fbo1->fb, src_w, src_h, fbo1->img.image, &pc, 128, scp,
					n_sc);
				current = tex1;
			}
		} else if (p->algorithm == BLUR_ALGORITHM_BOX) {
			union vk_push_data pc = {
				{
					{0}
				}
			};
			pc.gauss.texel[0] = 1.0f / src_w;
			pc.gauss.texel[1] = 1.0f / src_h;
			pc.gauss.radius = p->radius;
			pc.gauss._pad0 = 0;
			pc.gauss.vib = p->vibrancy;
			pc.gauss.vd = p->vibrancy_darkness;
			pc.gauss.br = p->brightness;
			pc.gauss.cont = p->contrast;
			vk_draw_full(vk->pipe_box_h, current, fbo0->fb, src_w, src_h, fbo0->img.image, &pc, 128, scp,
				n_sc);
			if (last) {
				vk_draw_full(vk->pipe_box_v, tex0, dst->fb, src_w, src_h, dst_img, &pc, 128, scp, n_sc);
				current = dst_img;
			} else {
				vk_draw_full(vk->pipe_box_v, tex0, fbo1->fb, src_w, src_h, fbo1->img.image, &pc, 128, scp,
					n_sc);
				current = tex1;
			}
		} else if (p->algorithm == BLUR_ALGORITHM_REFRACTION ||
				p->algorithm == BLUR_ALGORITHM_LENS_REFRACTION) {
			int mode = (p->algorithm == BLUR_ALGORITHM_LENS_REFRACTION) ? 1 : 0;
			struct vk_fbo *dfbo = (i & 1) ? fbo1 : fbo0;
			VkImage dimg = (i & 1) ? tex1 : tex0;
			if (last) {
				dfbo = dst;
				dimg = dst_img;
			}
			union vk_push_data pc = {
				{
					{0}
				}
			};
			pc.refraction.hp[0] = 0.5f / src_w;
			pc.refraction.hp[1] = 0.5f / src_h;
			pc.refraction.off = p->refraction_offset;
			pc.refraction.rsize[0] = (float)src_w;
			pc.refraction.rsize[1] = (float)src_h;
			float max_edge = 0.5f * (float)((src_w < src_h) ? src_w : src_h);
			float edge = p->refraction_edge_size_px;
			if (edge > max_edge)
				edge = max_edge;
			if (edge < 0)
				edge = 0;
			pc.refraction.edge = edge;
			float max_corner = 0.5f * (float)((src_w < src_h) ? src_w : src_h);
			float corner = p->refraction_corner_radius_px;
			if (corner > max_corner)
				corner = max_corner;
			if (corner < 0)
				corner = 0;
			pc.refraction.corner = corner;
			float sn = p->refraction_strength / 30.0f;
			if (sn < 0)
				sn = 0;
			if (sn > 1)
				sn = 1;
			pc.refraction.str = sn;
			pc.refraction.npow = p->refraction_normal_pow;
			float fringing = p->refraction_rgb_fringing;
			if (fringing < 0)
				fringing = 0;
			if (fringing > 1)
				fringing = 1;
			pc.refraction.rf = fringing;
			pc.refraction.rm = p->refraction_texture_repeat_mode;
			pc.refraction.mode = mode;
			vk_draw_full(vk->pipe_refraction, current, dfbo->fb, src_w, src_h, dfbo->img.image, &pc, 128, scp,
				n_sc);
			current = dimg;
		} else {
			struct vk_fbo *dfbo = (i & 1) ? fbo1 : fbo0;
			VkImage dimg = (i & 1) ? tex1 : tex0;
			if (last) {
				dfbo = dst;
				dimg = dst_img;
			}
			union vk_push_data pc = {
				{
					{0}
				}
			};
			pc.kawase.hp[0] = 0.5f / src_w;
			pc.kawase.hp[1] = 0.5f / src_h;
			pc.kawase.off = p->radius * (float)(i + 1);
			pc.kawase.noise = p->noise_strength;
			pc.kawase.vib = p->vibrancy;
			pc.kawase.vd = p->vibrancy_darkness;
			pc.kawase.br = p->brightness;
			pc.kawase.cont = p->contrast;
			vk_draw_full(vk->pipe_kawase, current, dfbo->fb, src_w, src_h, dfbo->img.image, &pc, 128, scp,
				n_sc);
			current = dimg;
		}
	}

	if (dst_fbo) {
		vk_transition_to_shader_read_after_draw(dst_img);
		if (out_handle)
			*out_handle = 0;
	} else {
		*out_handle = (uint64_t)current;
	}
	return true;
}

static bool vk_apply_mica_tint(be_output_state_t *state, uint64_t bg_handle, float tint[4],
		float tint_strength, uint64_t dst_fbo, int w, int h) {
	(void)state;
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;
	struct {
		float tint[4];
		float strength;
	} pc;
	memcpy(pc.tint, tint, 16);
	pc.strength = tint_strength;
	vk_draw_full(vk->pipe_mica, vk_img_of(bg_handle), dst->fb, w, h, dst->img.image, &pc, sizeof(pc),
		NULL, 0);
	return true;
}

static bool vk_apply_acrylic(be_output_state_t *state, uint64_t bg_handle,
		struct be_acrylic_params *p, uint64_t dst_fbo, int w, int h) {
	VkImage blurred = vk_img_of(bg_handle);
	struct vk_fbo *fbo0 = vk_fbo_of(state->ping.native_handle[0]);
	struct vk_fbo *fbo1 = vk_fbo_of(state->pong.native_handle[0]);
	VkImage tex0 = vk_img_of(state->ping.native_handle[1]);
	VkImage tex1 = vk_img_of(state->pong.native_handle[1]);
	int blur_w = state->ping.width > 0 ? state->ping.width : w;
	int blur_h = state->ping.height > 0 ? state->ping.height : h;

	if (p->blur_passes > 0) {
		VkImage current = vk_img_of(bg_handle);
		for (int i = 0; i < p->blur_passes; i++) {
			struct {
				float hp[2];
				float off;
				float _pad[28];
			} pc = {0};
			pc.hp[0] = 0.5f / blur_w;
			pc.hp[1] = 0.5f / blur_h;
			pc.off = p->blur_radius * (float)(i + 1);
			struct vk_fbo *dfbo = (i & 1) ? fbo1 : fbo0;
			VkImage dimg = (i & 1) ? tex1 : tex0;
			vk_draw_full(vk->pipe_kawase, current, dfbo->fb, blur_w, blur_h, dfbo->img.image, &pc, sizeof(pc),
				NULL, 0);
			current = dimg;
		}
		blurred = (p->blur_passes & 1) ? tex1 : tex0;
	}
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;

	struct {
		float tint[4];
		float strength;
		float noise;
		float res[2];
		float anchor[2];
	} pc;
	memcpy(pc.tint, p->tint, 16);
	pc.strength = p->tint_strength;
	pc.noise = p->noise_strength;
	pc.res[0] = p->res_w;
	pc.res[1] = p->res_h;
	pc.anchor[0] = p->light_anchor_x;
	pc.anchor[1] = p->light_anchor_y;
	vk_draw_full(vk->pipe_acrylic, blurred, dst->fb, w, h, dst->img.image, &pc, sizeof(pc), NULL, 0);
	return true;
}

static bool vk_render_shadow(struct be_shadow_params *p, uint64_t dst_fbo) {
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;
	struct {
		float res[2];
		float size;
		float _pad;
		float col[4];
		float br;
		float _pad2;
		float inner[2];
		float hole[2];
		float hsize[2];
	} pc;
	memset(&pc, 0, sizeof(pc));
	pc.res[0] = (float)p->buf_w;
	pc.res[1] = (float)p->buf_h;
	pc.size = p->shadow_size;
	memcpy(pc.col, p->shadow_color, 16);
	pc.br = p->border_radius;
	pc.inner[0] = p->inner_width;
	pc.inner[1] = p->inner_height;
	pc.hole[0] = p->hole_x;
	pc.hole[1] = (float)p->buf_h - p->hole_y - p->hole_height;
	pc.hsize[0] = p->hole_width;
	pc.hsize[1] = p->hole_height;

	vk_draw_full_no_tex(vk->pipe_shadow, dst->img.image, dst->fb, p->buf_w, p->buf_h, true, &pc,
		sizeof(pc), vk->pipe_layout, vk->dummy_ds);
	return true;
}

static bool vk_render_border(struct be_border_params *p, uint64_t dst_fbo) {
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;
	struct vk_border_ubo ubo;
	memset(&ubo, 0, sizeof(ubo));
	memcpy(ubo.gradient_colors, p->gradient_colors, sizeof(p->gradient_colors));
	memcpy(ubo.gradient2_colors, p->gradient2_colors, sizeof(p->gradient2_colors));
	ubo.gradient_count = p->gradient_count;
	ubo.gradient2_count = p->gradient2_count;
	ubo.gradient_angle = p->gradient_angle;
	ubo.gradient2_angle = p->gradient2_angle;
	ubo.gradient_lerp = p->gradient_lerp;
	memcpy(vk->border_ubo_map, &ubo, sizeof(ubo));

	struct {
		float res[2];
		float br;
		float bw;
		float scale;
		float _pad[3];
		float color[4];
	} pc;
	memset(&pc, 0, sizeof(pc));
	pc.res[0] = p->res_w;
	pc.res[1] = p->res_h;
	pc.br = p->border_radius;
	pc.bw = p->border_width_px;
	pc.scale = p->scale;
	memcpy(pc.color, p->border_color, 16);

	vk_draw_full_no_tex(vk->pipe_border, dst->img.image, dst->fb, p->buf_w, p->buf_h, true, &pc,
		sizeof(pc), vk->border_pipe_layout, vk->border_ds);
	return true;
}

static VkImageView vk_lookup_or_create_view(VkImage image) {
	int s = vk->frame_slot;
	for (int i = 0; i < vk->n_cached_views[s]; i++) {
		if (vk->cached_images[s][i] == image)
			return vk->cached_views[s][i];
	}
	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = vk->vk_fmt,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	VkImageView view;
	if (vkCreateImageView(vk->device, &vci, NULL, &view) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	if (vk->n_cached_views[s] < VK_VIEW_CACHE_SIZE) {
		vk->cached_images[s][vk->n_cached_views[s]] = image;
		vk->cached_views[s][vk->n_cached_views[s]] = view;
		vk->n_cached_views[s]++;
	} else {
		vk_defer_view(view);
	}
	return view;
}

static bool vk_apply_corner_mask(be_output_state_t *state, uint64_t dst_fbo, int dst_w, int dst_h,
		uint64_t bg_tex, struct be_corner_mask_params *p) {
	(void)state;
	vk->frame_dirty = true;
	vk_ensure_cb_begun();
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;

	VkImage src_img = vk_img_of(bg_tex);
	VkImageView src_view = vk_lookup_or_create_view(src_img);
	if (src_view == VK_NULL_HANDLE)
		return false;

	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = vk->desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk->ds_layout,
	};
	VkDescriptorSet ds;
	if (vkAllocateDescriptorSets(vk->device, &dsai, &ds) != VK_SUCCESS) {
		wlr_log(WLR_DEBUG, "vk: corner mask descriptor pool exhausted, skipping");
		return false;
	}
	VkDescriptorImageInfo dii = {
		.sampler = vk->sampler,
		.imageView = src_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dii,
	};
	vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);

	struct {
		float wpos[2];
		float wsz[2];
		float wpx[2];
		float br;
		float scale;
		int pre;
		int pad;
		float bgpos[2];
		float bgsz[2];
	} pc;
	memset(&pc, 0, sizeof(pc));
	pc.wpos[0] = p->win_u;
	pc.wpos[1] = 1.0f - p->win_v - p->win_sh;
	pc.wsz[0] = p->win_sw;
	pc.wsz[1] = p->win_sh;
	pc.wpx[0] = p->win_size_px_w;
	pc.wpx[1] = p->win_size_px_h;
	pc.br = p->border_radius_px;
	pc.scale = p->scale;
	pc.pre = p->pre_blit ? 1 : 0;
	pc.bgpos[0] = p->bg_u;
	pc.bgpos[1] = 1.0f - p->bg_v - p->bg_sh;
	pc.bgsz[0] = p->bg_sw;
	pc.bgsz[1] = p->bg_sh;

	VkClearValue clear_val = {
		{
			{0.0f, 0.0f, 0.0f, 0.0f}
		}
	};

	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.image = dst->img.image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);

	VkRenderPassBeginInfo rp = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = p->pre_blit ? vk->overlay_render_pass : vk->color_clear_render_pass,
		.framebuffer = dst->fb,
		.renderArea = {
			{0, 0},
			{
				(uint32_t)dst_w,
				(uint32_t)dst_h
			}
		},
		.clearValueCount = p->pre_blit ? 0 : 1,
		.pClearValues = p->pre_blit ? NULL : &clear_val,
	};
	VkViewport vp = {
		0,
		(float)dst_h,
		(float)dst_w,
		-(float)dst_h,
		0,
		1
	};
	VkRect2D full_sc = {
		{0, 0},
		{
			(uint32_t)dst_w,
			(uint32_t)dst_h
		}
	};
	vkCmdBeginRenderPass(vk->frame_cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(vk->frame_cb, 0, 1, &vp);
	vkCmdBindDescriptorSets(vk->frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipe_layout, 0, 1, &ds,
		0, NULL);
	vkCmdPushConstants(vk->frame_cb, vk->pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
		&pc);
	vkCmdBindPipeline(vk->frame_cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		p->pre_blit ? vk->pipe_corner_mask : vk->pipe_corner_mask_clear);
	vkCmdSetScissor(vk->frame_cb, 0, 1, &full_sc);
	vkCmdDraw(vk->frame_cb, 4, 1, 0, 0);
	vkCmdEndRenderPass(vk->frame_cb);

	return true;
}

static bool vk_apply_screen_shader(uint64_t src_tex, uint64_t dst_fbo, int w, int h,
		struct be_screen_shader_params *p) {
	if (!vk->screen_shader_pipe)
		return false;
	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst)
		return false;
	VkImage src_img = vk_img_of(src_tex);

	struct {
		float res[2];
		float time;
	} pc;
	pc.res[0] = (float)w;
	pc.res[1] = (float)h;
	pc.time = p->time;

	vk_draw_full(vk->screen_shader_pipe, src_img, dst->fb, w, h, dst->img.image, &pc, sizeof(pc), NULL,
		0);
	return true;
}

static bool vk_capture_readback(struct wlr_buffer *capture_buffer, be_output_state_t *state,
		uint64_t dst_fbo, int dst_x, int dst_y, int dst_w, int dst_h, int src_x, int src_y, int src_w,
		int src_h, uint64_t *out_tex) {
	vk->frame_dirty = true;
	vk_ensure_cb_begun();
	(void)src_w;
	(void)src_h;
	struct wlr_texture *tex = wlr_texture_from_buffer(vk->wlr_renderer, capture_buffer);
	if (!tex) {
		wlr_log(WLR_ERROR, "vk: capture_readback: no texture");
		return false;
	}

	struct wlr_vk_image_attribs vk_attribs;
	wlr_vk_texture_get_image_attribs(tex, &vk_attribs);

	struct vk_fbo *dst = vk_fbo_of(dst_fbo);
	if (!dst) {
		wlr_texture_destroy(tex);
		return false;
	}

	VkImageLayout src_layout = vk_attribs.layout;

	VkImage result_img = vk_img_of(state->pong.native_handle[1]);
	if (dst == vk_fbo_of(state->screen_shader.native_handle[0]))
		result_img = vk_img_of(state->screen_shader.native_handle[1]);

	VkImageMemoryBarrier src_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = src_layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = vk_attribs.image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &src_barrier);

	VkImageMemoryBarrier dst_barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = dst->img.image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &dst_barrier);

	int capture_w = src_w > 0 ? src_w : dst_w;
	int capture_h = src_h > 0 ? src_h : dst_h;
	VkImageBlit region = {
		.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.srcOffsets = {
			{src_x, src_y, 0},
			{src_x + capture_w, src_y + capture_h, 1}
		},
		.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.dstOffsets = {
			{dst_x, dst_y, 0},
			{dst_x + dst_w, dst_y + dst_h, 1}
		},
	};
	vkCmdBlitImage(vk->frame_cb, vk_attribs.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

	VkImageMemoryBarrier post_barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.image = vk_attribs.image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.image = dst->img.image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
		},
	};
	vkCmdPipelineBarrier(vk->frame_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, post_barriers);
	wlr_texture_destroy(tex);
	*out_tex = (uint64_t)result_img;
	return true;
}

static bool vk_compile_screen_shader(const char *frag_src) {
	if (vk->screen_shader_pipe) {
		vkDestroyPipeline(vk->device, vk->screen_shader_pipe, NULL);
		vk->screen_shader_pipe = VK_NULL_HANDLE;
	}
	if (vk->screen_shader_module) {
		vkDestroyShaderModule(vk->device, vk->screen_shader_module, NULL);
		vk->screen_shader_module = VK_NULL_HANDLE;
	}

	static const struct {
		const char *from;
		const char *to;
	} subs[] = {
		{"precision mediump float;", ""},
		{"precision highp float;", ""},
		{"precision lowp float;", ""},
		{"varying vec2 v_uv;", "layout(location = 0) in vec2 v_uv;"},
		{"varying vec4 v_uv;", "layout(location = 0) in vec4 v_uv;"},
		{"attribute vec2 pos;", ""},
		{"attribute vec4 pos;", ""},
		{"uniform sampler2D tex;", "layout(binding = 0) uniform sampler2D tex;"},
		{"texture2D(", "texture("},
		{"gl_FragColor", "fragColor"},
		{"#extension", "//"},
		{"#include", "//"},
	};
	const char *header = "#version 450\nlayout(location = 0) out vec4 fragColor;\n";
	size_t len = strlen(header) + strlen(frag_src) + 2048;
	char *converted = malloc(len);
	if (!converted)
		return false;
	strcpy(converted, header);
	char *dst = converted + strlen(header);
	const char *src = frag_src;
	while (*src) {
		bool matched = false;
		for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
			size_t sl = strlen(subs[i].from);
			if (strncmp(src, subs[i].from, sl) == 0) {
				size_t tl = strlen(subs[i].to);
				memcpy(dst, subs[i].to, tl);
				dst += tl;
				src += sl;
				matched = true;
				break;
			}
		}
		if (!matched)
			*dst++ = *src++;
	}
	*dst = '\0';

	VkShaderModule mod = vk_compile_shader(converted, VK_SHADER_STAGE_FRAGMENT_BIT);
	free(converted);
	if (!mod)
		return false;
	vk->screen_shader_module = mod;

	VkPipeline pipe = vk_create_pipe(mod, false, false);
	if (!pipe) {
		vkDestroyShaderModule(vk->device, mod, NULL);
		vk->screen_shader_module = VK_NULL_HANDLE;
		return false;
	}
	vk->screen_shader_pipe = pipe;
	return true;
}

static void vk_destroy_screen_shader(void) {
	if (vk->screen_shader_module) {
		vkDestroyShaderModule(vk->device, vk->screen_shader_module, NULL);
		vk->screen_shader_module = VK_NULL_HANDLE;
	}
	if (vk->screen_shader_pipe) {
		vkDestroyPipeline(vk->device, vk->screen_shader_pipe, NULL);
		vk->screen_shader_pipe = VK_NULL_HANDLE;
	}
}

static const char *vk_get_screen_shader_source(const char *name) {
	if (strcmp(name, "grayscale") == 0)
		return vk_grayscale_frag_src;
	if (strcmp(name, "invert") == 0)
		return vk_invert_frag_src;
	if (strcmp(name, "sepia") == 0)
		return vk_sepia_frag_src;
	if (strcmp(name, "nightlight") == 0)
		return vk_nightlight_frag_src;
	return NULL;
}

const effects_backend_t vk_backend = {
	.init = vk_init,
	.fini = vk_fini,
	.output_init = vk_output_init,
	.output_fini = vk_output_fini,
	.output_resize = vk_output_resize,
	.ensure_buffer = vk_ensure_buffer,
	.destroy_buffer = vk_destroy_buffer,
	.frame_begin = vk_frame_begin,
	.frame_end = vk_frame_end,
	.blit = vk_blit,
	.blur = vk_blur,
	.apply_mica_tint = vk_apply_mica_tint,
	.apply_acrylic = vk_apply_acrylic,
	.render_shadow = vk_render_shadow,
	.render_border = vk_render_border,
	.apply_corner_mask = vk_apply_corner_mask,
	.apply_screen_shader = vk_apply_screen_shader,
	.capture_readback = vk_capture_readback,
	.get_screen_shader_source = vk_get_screen_shader_source,
	.compile_screen_shader = vk_compile_screen_shader,
	.destroy_screen_shader = vk_destroy_screen_shader,
};


#endif
