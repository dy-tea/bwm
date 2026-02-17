project(
	'bwm',
	'c',
	default_options: ['warning_level=3'],
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wayland_protos = dependency('wayland-protocols')

libs = [
	dependency('wlroots-0.20'),
	dependency('wayland-server'),
	dependency('xkbcommon'),
	dependency('libinput'),
	wayland_protos,
]

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

protocols = [
	wl_protocol_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
	'protocols' / 'wlr-layer-shell-unstable-v1.xml',
]

wl_protos_src = []

foreach xml : protocols
	wl_protos_src += custom_target(
		xml.underscorify() + '_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	wl_protos_src += custom_target(
		xml.underscorify() + '_server_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
	wl_protos_src += custom_target(
		xml.underscorify() + '_client_h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

inc = include_directories(['include'])

executable(
	'bwm',
	[
		'src' / 'main.c',
		'src' / 'server.c',
		'src' / 'output.c',
		'src' / 'tree.c',
		'src' / 'toplevel.c',
		'src' / 'popup.c',
		'src' / 'keyboard.c',
		'src' / 'transaction.c',
		'src' / 'workspace.c',
		'src' / 'ipc.c',
		'src' / 'layer.c',
		wl_protos_src,
	],
	include_directories: inc,
	dependencies: libs,
)

executable(
	'bmsg',
	'bmsg' / 'bmsg.c',
)
