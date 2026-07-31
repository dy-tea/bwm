# doors

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
- shaderc
- vulkan

Build with meson:
```
meson setup build
ninja -C build
```

### Configuration

You can use the example config under [examples](examples).
These should be placed under `$XDG_CONFIG_HOME/doors/` (or `$HOME/.config/doors/`), or you can pass the directory you have put them in with the -c arg like:
```
doors -c ./examples/
```

The **doorsrc** is a bash file that is run at startup, and can be used to configure settings and launch applications (like an **.xinitrc**).

Use `doorsctl env set <name> <value>` in `doorsrc` to update the compositor's environment. Applications launched later by Doors, including hotkey commands, inherit these values.

The **doorshkrc** is a config file for your hotkeys, and is reloaded on every save. It uses a config format similar to [sxhkd](https://github.com/baskerville/sxhkd) and can execute compositor binds by calling the `doorsctl` command.

### Packages

If you would like to install as a package, an arch package is available on the AUR at [doors-git](https://aur.archlinux.org/packages/doors-git).

There is also a nix flake maintained by [anispwn](https://github.com/anispwyn).
