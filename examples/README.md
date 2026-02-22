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
bmsg config gapless_monocle true|false
```

When true, windows are rearranged to fill gaps when windows are closed.

### Monitor/Desktop Setup

```
bmsg monitor -d <name1> <name2> ...
```

Configures desktops on the focused monitor. Desktop names are space-separated.
The number of desktops is determined by the number of names provided.

```
bmsg monitor <name> -d <name1> <name2> ...
```

Configures desktops on a specific monitor by name.

## bwmhkrc (Hotkey Config)

Hotkeys are defined with one key binding per line, followed by indented command(s).

Format:
```
<modifiers> + <key>
	<command>
```

### Modifiers

- `alt` - Alt key (Mod1)
- `ctrl` or `control` - Control key
- `shift` - Shift key
- `super` or `Mod4` - Super/Win key
- `mod4` - Mod4 key

Combine modifiers with `+`, e.g., `alt + shift + q`

### Commands

All commands use the `bmsg` prefix. Commands not starting with `bmsg` are executed as shell commands.

#### Node/Window Commands

```
bmsg node --close                  # Close focused window
bmsg node --focus                  # Focus most recently focused window
bmsg node --state tiled            # Set focused window to tiled
bmsg node --state floating         # Set focused window to floating
bmsg node --state fullscreen       # Set focused window to fullscreen
bmsg node --to-desktop <n>         # Send window to desktop n (1-10)
bmsg node --to-desktop <name>      # Send window to named desktop
```

#### Desktop Commands

```
bmsg desktop <n>                   # Switch to desktop n (1-10)
bmsg desktop <name>                # Switch to named desktop
bmsg desktop next                  # Switch to next desktop
bmsg desktop prev                  # Switch to previous desktop
bmsg desktop --focus <name>        # Focus named desktop
bmsg desktop --focus next          # Focus next desktop
bmsg desktop --focus prev          # Focus previous desktop
bmsg desktop --layout tiled        # Set desktop to tiled layout
bmsg desktop --layout monocle       # Set desktop to monocle layout
bmsg desktop --rename <newname>     # Rename desktop
```

#### Focus Commands

```
bmsg focus west|w                  # Focus window to the left
bmsg focus east|e                  # Focus window to the right
bmsg focus north|n                 # Focus window above
bmsg focus south|s                  # Focus window below
```

#### Swap Commands

```
bmsg swap west|w                    # Swap with window to the left
bmsg swap east|e                    # Swap with window to the right
bmsg swap north|n                   # Swap with window above
bmsg swap south|s                   # Swap with window below
```

#### Preselection Commands

```
bmsg presel west|w                  # Preselect left direction for next window
bmsg presel east|e                   # Preselect right direction for next window
bmsg presel north|n                  # Preselect above direction for next window
bmsg presel south|s                  # Preselect below direction for next window
bmsg presel cancel                   # Cancel current preselection
```

#### Toggle Commands

```
bmsg toggle floating                # Toggle focused window floating/tiled
bmsg toggle fullscreen              # Toggle focused window fullscreen
bmsg toggle monocle                 # Toggle monocle layout on desktop
```

#### Rotate/Flip Commands

```
bmsg rotate clockwise|cw           # Rotate window layout clockwise
bmsg rotate counterclockwise|ccw   # Rotate window layout counter-clockwise
bmsg flip horizontal|h             # Flip window layout horizontally
bmsg flip vertical|v               # Flip window layout vertically
```

#### Send Commands

```
bmsg send next                      # Send focused window to next desktop
bmsg send prev|previous             # Send focused window to previous desktop
```

#### Other Commands

```
bmsg quit                           # Quit the compositor
<shell command>                     # Execute any shell command (no bmsg prefix)
```

## IPC Commands (Advanced)

These commands can be used in scripts via `bmsg`:

### Output Commands

```
bmsg output <name> enable
bmsg output <name> disable
bmsg output <name> mode <width>x<height>[@<refresh>]
bmsg output <name> position <x> <y>
bmsg output <name> scale <factor>
bmsg output <name> transform <normal|90|180|270|flipped|flipped-90|flipped-270>
bmsg output <name> dpms on|off
bmsg output <name> adaptive_sync on|off
bmsg output <name> render_bit_depth 8|10
```

### Input Commands

Target specific devices:
```
bmsg input <device-name> <property> <value>
bmsg input type:keyboard <property> <value>
bmsg input type:pointer <property> <value>
bmsg input type:touchpad <property> <value>
bmsg input type:touchscreen <property> <value>
bmsg input * <property> <value>
```

Keyboard properties:
- `xkb_layout` - Keyboard layout (e.g., "us", "gb", "de")
- `xkb_model` - Keyboard model
- `xkb_options` - XKB options (e.g., "caps:escape")
- `xkb_rules` - XKB rules
- `xkb_variant` - XKB variant
- `xkb_file` - Path to XKB keymap file
- `repeat_rate` - Key repeat rate (characters per second)
- `repeat_delay` - Key repeat delay (ms)
- `xkb_numlock` - Enable numlock (true/false)
- `xkb_capslock` - Enable capslock (true/false)

Pointer/Touchpad properties:
- `pointer_accel` - Pointer acceleration (-1 to 1)
- `accel_profile` - flat, adaptive
- `natural_scroll` - Natural scrolling (true/false)
- `left_handed` - Left-handed mouse (true/false)
- `tap` - Tap to click (true/false)
- `tap_button_map` - lrm, lmr
- `drag` - Drag to scroll (true/false)
- `drag_lock` - Drag lock (true/false)
- `dwt` - Disable while typing (true/false)
- `dwtp` - Disable while typing on palm (true/false)
- `click_method` - button_areas, clickfinger, none
- `middle_emulation` - Middle mouse button emulation (true/false)
- `scroll_method` - edge, button, twofinger, none
- `scroll_button` - Mouse button for scroll (scancode or buttonN)
- `scroll_button_lock` - Lock scroll button (true/false)
- `scroll_factor` - Scroll speed multiplier
- `rotation_angle` - Pointer rotation (degrees)

### Monitor Commands

```
bmsg monitor <name> -f                 # Focus monitor
bmsg monitor <name> -n <newname>      # Rename monitor
bmsg monitor <name> -a <desk1> ...    # Add desktops
bmsg monitor <name> -d <desk1> ...    # Reset desktops
monitor -l                            # List all monitors
```

### Query Commands

```
bmsg query -T --tree      # Get window tree
bmsg query -M --monitors # List monitors
bmsg query -D --desktops # List desktops
bmsg query -N --nodes    # List node IDs
```

### Config Commands

```
bmsg config border_width [<n>]    # Get or set border width
bmsg config window_gap [<n>]     # Get or set window gap
bmsg config single_monocle [true|false]
bmsg config borderless_monocle [true|false]
bmsg config gapless_monocle [true|false]
```
