# bwm

_Wayland compositor based on bspwm_

### Building

Ensure you have the following dependencies installed:

- wlroots-git (latest git)
- wayland
- wayland-protocols
- xkbcommon
- libinput
- pixman-1
- xcb
- xcb-icccm
- glesv2
- egl
- cairo
- pangocairo

Build with meson:
```
meson setup build
ninja -C build
```

### Configuration

You can use the example config under [examples](examples).
These should be placed under `$XDG_CONFIG_HOME/bwm/` (or `$HOME/.config/bwm/`), or you can pass the directory you have put them in with the -c arg like:
```
bwm -c ./examples/
```

The **bwmrc** is a bash file that is run at startup, and can be used to configure settings and launch applications (like an **.xinitrc**).

The **bwmhkrc** is a config file for your hotkeys, and is reloaded on every save. It uses a config format similar to [sxhkd](https://github.com/baskerville/sxhkd) and can execute compositor binds by calling the `bmsg` command.
