embed = find_program('./embed.pl', native: true)

# GLES2
gles_shaders = [
	'effect_tex.vert',

	'blit.frag',
	'ext_blit.frag',

	# Border
	'border.frag',
	'border_corner_mask.frag',

	# Window blur
	'blur_kawase.frag',
	'blur_box_h.frag',
	'blur_box_v.frag',
	'blur_gauss_h.frag',
	'blur_gauss_v.frag',
	'blur_mica.frag',
	'blur_acrylic.frag',
	'blur_refraction.frag',

	# Shadows
	'shadow.frag',

	# Fullscreen effects
	'grayscale.frag',
	'invert.frag',
	'nightlight.frag',
	'sepia.frag',
]

foreach shader : gles_shaders
	output = 'gl_' + shader.underscorify() + '_src.h'
	var = 'gl_' + shader.underscorify() + '_src'
	shader_headers += custom_target(
		output,
		command: [embed, var, 'gles2'],
		input: 'gles2' / shader,
		output: output,
		feed: true,
		capture: true,
	)
endforeach

# Vulkan
vk_shaders = [
	'effect_tex.vert',

	'blit.frag',

	# Border
	'border.frag',
	'border_corner_mask.frag',

	# Window blur
	'blur_kawase.frag',
	'blur_box_h.frag',
	'blur_box_v.frag',
	'blur_gauss_h.frag',
	'blur_gauss_v.frag',
	'blur_mica.frag',
	'blur_acrylic.frag',
	'blur_refraction.frag',

	# Shadows
	'shadow.frag',

	# Fullscreen effects
	'grayscale.frag',
	'invert.frag',
	'nightlight.frag',
	'sepia.frag',
]

foreach shader : vk_shaders
	output = 'vk_' + shader.underscorify() + '_src.h'
	var = 'vk_' + shader.underscorify() + '_src'
	shader_headers += custom_target(
		output,
		command: [embed, var, 'vulkan'],
		input: 'vulkan' / shader,
		output: output,
		feed: true,
		capture: true,
	)
endforeach
