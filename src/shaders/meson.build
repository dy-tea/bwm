embed = find_program('./embed.pl', native: true)

shaders = [
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

	# Fullscreen effects
	'grayscale.frag',
	'invert.frag',
	'nightlight.frag',
	'sepia.frag',
]

foreach shader : shaders
	output = shader.underscorify() + '_src.h'
	var = shader.underscorify() + '_src'
	shader_headers += custom_target(
		output,
		command: [embed, var],
		input: shader,
		output: output,
		feed: true,
		capture: true,
	)
endforeach
