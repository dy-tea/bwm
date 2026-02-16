project(
  'bwm',
  'c',
  default_options: ['warning_level=3'],
)

libs = [
  dependency('wlroots-0.20'),
  dependency('wayland-server'),
  dependency('xkbcommon'),
  dependency('libinput'),
]

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
  ],
  include_directories: include_directories('include'),
  dependencies: libs,
)

executable(
  'bmsg',
  'bmsg' / 'bmsg.c',
)
