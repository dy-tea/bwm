# BWM Configuration Documentation

BWM uses two configuration files located in `~/.config/bwm/`:

- `bwmrc` - Startup script executed when the compositor starts
- `bwmhkrc` - Hotkey bindings configuration

Both files use `bmsg` to communicate with the running compositor via IPC.

## bwmrc (Startup Config)

This file is executed as a shell script at server startup. Use it to configure
initial settings and set up desktops.

### Config Settings

```
bmsg config border_width <pixels>
```

Sets the border width around windows.

```
bmsg config window_gap <pixels>
```

Sets the gap between windows.

```
bmsg config single_monocle true|false
```

When true, monocle layout shows only the focused window regardless of how many
windows are on the desktop.

```
bmsg config borderless_monocle true|false
```

When true, removes borders in monocle layout.

```
bmsg config borderless_singleton true|false
```

When true, removes borders for singleton windows.

```
bmsg config gapless_monocle true|false
```

When true, windows are rearranged to fill gaps when windows are closed.

```
bmsg config edge_scroller_pointer_focus true|false
```

When true, pointer focus affects scroller behavior at edges.

```
bmsg config scroller_default_proportion <value>
```

Sets the default proportion for scroller layout (0.1-1.0).

```
bmsg config scroller_proportion_preset [<values>]
```

### Blur Settings

BWM supports window background blur effects using OpenGL shaders. Three blur algorithms are available: `kawase` (default), `gaussian`, and `box`.

```
bmsg config blur_enabled true|false
```

Enables or disables the blur effect (default: true).

```
bmsg config blur_algorithm none|kawase|gaussian|box
```

Sets the blur algorithm (default: kawase).

```
bmsg config blur_passes <n>
```

Sets the number of blur passes (1-10, default: 3). Higher values create stronger blur but require more GPU processing.

```
bmsg config blur_radius <value>
```

Sets the blur radius (default: 8.0). Higher values create stronger blur but may cause artifacts.

### Mica Settings

Mica is a background effect that captures and tints the content behind windows.

```
bmsg config mica_enabled true|false
```

Enables or disables mica effect (default: false).

```
bmsg config mica_tint_strength <value>
```

Sets the tint strength for mica effect (0.0-1.0, default: 0.35).

```
bmsg config mica_tint <R> <G> <B> [A]
```

Sets the tint color for mica effect (default: 0.12 0.12 0.14 1.0). RGBA values should be in the range 0.0-1.0.

### WM Commands

```
bmsg wm --dump-state               # Dump current WM state as JSON
bmsg wm --load-state               # Load WM state (not implemented)
bmsg wm --add-monitor <name>       # Add a new monitor
```

### Subscribe Commands

```
bmsg subscribe [-c <count>] [-f <fifo>] <event>...
```

Subscribe to WM events (advanced usage).

### Keyboard Grouping Commands

```
bmsg keyboard_grouping none|smart|default
```

Set keyboard grouping mode.

### Scroller Commands

```
bmsg scroller proportion <value>   # Set proportion for focused scroller client
bmsg scroller stack                # Stack focused client with previous
```

### Equalize/Balance Commands

```
bmsg equalize # Equalize window sizes in tree
bmsg balance  # Balance window sizes in tree
```
