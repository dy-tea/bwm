project(
	'doors',
	'c',
	default_options: ['warning_level=3'],
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wayland_protos = dependency('wayland-protocols')

wlroots_version = ['>=0.21.0', '<0.22.0']
subproject(
	'wlroots',
	default_options: ['examples=false'],
	required: false,
	version: wlroots_version,
)
wlroots = dependency('wlroots-0.21', version: wlroots_version, fallback: 'wlroots')

libs = [
	wlroots,
	dependency('wayland-server'),
	dependency('xkbcommon'),
	dependency('libinput'),
	dependency('pixman-1'),
	dependency('xcb'),
	dependency('xcb-icccm'),
	dependency('glesv2'),
	dependency('egl'),
	dependency('cairo'),
	dependency('pangocairo'),
	dependency('vulkan'),
	dependency('shaderc'),
	wayland_protos,
]

libsystemd = dependency('libsystemd', required: false)
if libsystemd.found()
  libs += libsystemd
  add_project_arguments('-DHAVE_SYSTEMD=1', language: 'c')
endif

if host_machine.system() == 'freebsd'
  libs += declare_dependency(link_args: ['/usr/local/lib/libinotify.so', '-lpthread'])
endif

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

protocols = [
	wl_protocol_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
	wl_protocol_dir / 'unstable' / 'text-input' / 'text-input-unstable-v3.xml',
	'protocols' / 'wlr-layer-shell-unstable-v1.xml',
	'protocols' / 'wlr-output-power-management-unstable-v1.xml',
	'protocols' / 'wlr-screencopy-unstable-v1.xml',
	'protocols' / 'ext-image-capture-source-v1.xml',
	'protocols' / 'ext-image-copy-capture-v1.xml',
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

shader_headers = []
subdir('src' / 'shaders')

inc = include_directories(['include'])

executable(
	'doors',
	[
		'src' / 'animation.c',
		'src' / 'bezier.c',
		'src' / 'config.c',
		'src' / 'copy_capture.c',
		'src' / 'cursor.c',
		'src' / 'effects.c',
		'src' / 'effects_gles2.c',
		'src' / 'effects_vk.c',
		'src' / 'gesture.c',
		'src' / 'global_shortcuts.c',
		'src' / 'idle.c',
		'src' / 'idle_power.c',
		'src' / 'ipc.c',
		'src' / 'ipc_cmd' / 'actions.c',
		'src' / 'ipc_cmd' / 'config.c',
		'src' / 'ipc_cmd' / 'env.c',
		'src' / 'ipc_cmd' / 'desktop.c',
		'src' / 'ipc_cmd' / 'dispatch.c',
		'src' / 'ipc_cmd' / 'hotkey.c',
		'src' / 'ipc_cmd' / 'input.c',
		'src' / 'ipc_cmd' / 'node.c',
		'src' / 'ipc_cmd' / 'output.c',
		'src' / 'ipc_cmd' / 'query.c',
		'src' / 'ipc_cmd' / 'quit.c',
		'src' / 'ipc_cmd' / 'rule.c',
		'src' / 'ipc_cmd' / 'scratchpad.c',
		'src' / 'ipc_cmd' / 'scroller.c',
		'src' / 'ipc_cmd' / 'seat.c',
		'src' / 'ipc_cmd' / 'wm.c',
		'src' / 'ipc_helpers.c',
		'src' / 'ipc_subscribe.c',
		'src' / 'input.c',
		'src' / 'input_method.c',
		'src' / 'keyboard.c',
		'src' / 'launcher.c',
		'src' / 'layout.c',
		'src' / 'layer.c',
		'src' / 'lock.c',
		'src' / 'log.c',
		'src' / 'main.c',
		'src' / 'master_stack.c',
		'src' / 'output.c',
		'src' / 'output_config.c',
		'src' / 'popup.c',
		'src' / 'realtime.c',
		'src' / 'render_unfocused.c',
		'src' / 'rule.c',
		'src' / 'scratchpad.c',
		'src' / 'screencopy.c',
		'src' / 'scroller.c',
		'src' / 'seat.c',
		'src' / 'server.c',
		'src' / 'spring.c',
		'src' / 'surface.c',
		'src' / 'tabs.c',
		'src' / 'tablet.c',
		'src' / 'tearing.c',
		'src' / 'text.c',
		'src' / 'transaction.c',
		'src' / 'tiling_drag.c',
		'src' / 'toplevel.c',
		'src' / 'tree.c',
		'src' / 'workspace.c',
		'src' / 'xwayland.c',
		wl_protos_src,
		shader_headers,
	],
	include_directories: inc,
	dependencies: libs,
	install: true,
	install_dir: get_option('bindir'),
)

executable(
	'doorsctl',
	'doorsctl' / 'doorsctl.c',
	install: true,
	install_dir: get_option('bindir'),
)

install_data('doors.desktop', install_dir: '/usr/share/wayland-sessions')

if get_option('SYSTEMD')
	install_data('arch' / 'doors-session.target', install_dir: '/usr/lib/systemd/user')
endif
