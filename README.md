# bwm

_Wayland compositor based on bswpm_

### Building

Ensure you have the following dependencies installed:

- wlroots-git (latest git)
- wayland
- wayland-protocols
- xkbcommon
- libinput
- pixman-1

Build with meson:
```
meson setup build
ninja -C build
```

### Configuration

You can use the example config under [examples](examples).
These should be placed under `$XDG_CONFIG_HOME/bwm/`, or you can pass the directory you have put them in with the -c arg like:
```
bwm -c ./examples/
```
