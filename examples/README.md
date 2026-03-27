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

Sets preset proportions for scroller layout (comma-separated).

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

### Mouse Button Bindings

Mouse buttons can be used in hotkey bindings for both custom commands and built-in interactions on floating windows.

#### Mouse Button Names

- `mouse_left` - Left mouse button
- `mouse_right` - Right mouse button
- `mouse_middle` - Middle mouse button (scroll wheel click)
- `mouse_back` - Back button
- `mouse_forward` - Forward button

#### Floating Window Interactions

When holding Alt with a mouse button on a floating window, bwm provides built-in interactive operations:

```
alt + mouse_left   # Interactive move (drag to reposition)
alt + mouse_right  # Interactive resize (drag to resize, edges detected from cursor position)
```

The resize operation detects which edge(s) to resize based on cursor position (10px border threshold), defaulting to bottom-right if clicking in the center.

#### Custom Mouse Button Binds

Mouse buttons can also be used with modifiers for custom keybinds:

```
alt + ctrl + mouse_right
	foot
```

### Commands

All commands use the `bmsg` prefix. Commands not starting with `bmsg` are executed as shell commands.

#### Node/Window Commands

```
bmsg node --close                  						# Close focused window
bmsg node --focus                  						# Focus most recently focused window
bmsg node --state tiled            						# Set focused window to tiled
bmsg node --state floating         						# Set focused window to floating
bmsg node --state fullscreen       						# Set focused window to fullscreen
bmsg node --to-desktop <n>         						# Send window to desktop n (1-10)
bmsg node --to-desktop <name>      						# Send window to named desktop
bmsg node --flag hidden=true       						# Toggle hidden flag on window
bmsg node --flag sticky=on         						# Toggle sticky flag on window
bmsg node --flag private=off       						# Toggle private flag on window
bmsg node --flag locked=true       						# Toggle locked flag on window
bmsg node --flag marked=off        						# Toggle marked flag on window
bmsg node --move <dx> <dy>         						# Move floating window by delta x,y
bmsg node --resize <handle> <dx> <dy>  				# Resize floating window (handle: northwest/nw/north/n/ne/east/e/southeast/se/south/s/southwest/sw/west/w/center/c)
bmsg node --activate               						# Activate focused window
bmsg node --kill                   						# Kill focused window
bmsg node --to-monitor <name>      						# Send window to monitor
bmsg node --to-node <id>           						# Send window to node with id
bmsg node --layer below|normal|above  				# Set window layer
bmsg node --type horizontal|vertical  				# Set split type for container
bmsg node --ratio <value>          						# Set split ratio for container
bmsg node --circulate forward|backward  			# Circulate focus in tree
bmsg node --insert-receptacle      						# Insert receptacle at focused node
bmsg node --presel-dir west|east|north|south  # Preselect direction for next window
bmsg node --presel-ratio <value>   						# Set preselection ratio
bmsg node --swap <id>              						# Swap with node by id
bmsg node interactive_move         						# Interactive move (for mouse button binds on floating windows)
bmsg node interactive_resize       						# Interactive resize (for mouse button binds on floating windows)
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
bmsg toggle pseudo_tiled            # Toggle focused window pseudo-tiled
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

#### Window Rules

Window rules allow you to control how specific applications are managed when they open.

```
bmsg rule -a <app_id> [options...]  # Add a new rule
bmsg rule -r <index>                # Remove rule by index
bmsg rule -l                        # List all rules
```

**Adding Rules (`-a` / `--add`):**
Specify an app_id (or title) followed by consequence options:

```
bmsg rule -a dev.zed.Zed desktop=I
bmsg rule -a zen desktop=II
bmsg rule -a vesktop desktop=III
```

**Consequence Options:**

- `desktop=<name>` - Send window to specified desktop (e.g., `desktop=I`, `desktop=^1` for first desktop on monitor 1)
- `state=<state>` - Set window state: `tiled`, `floating`, `fullscreen`, `pseudo_tiled`
- `follow=on|off` - Whether focus follows the window to its desktop (default: on)
- `focus=on|off` - Whether the window receives focus (default: on)
- `manage=on|off` - Whether the window is managed by the compositor
- `locked=on|off` - Lock window to its desktop
- `hidden=on|off` - Start window hidden
- `sticky=on|off` - Make window sticky (visible on all desktops)
- `one_shot` - Remove rule after first match


**Matching by Title:**

```
bmsg rule -a title="Exact Window Title" desktop=I
```

**Examples:**

```
# Send Zed to desktop I, don't switch focus away from current desktop
bmsg rule -a dev.zed.Zed desktop=I follow=off

# Send Firefox to desktop II as floating window
bmsg rule -a firefox desktop=II state=floating

# Ignore a window (don't manage it)
bmsg rule -a some-app manage=off

# Start a window in fullscreen
bmsg rule -a mpv state=fullscreen

# List all configured rules
bmsg rule -l

# Remove rule at index 0
bmsg rule -r 0
```

### Gesture Bindings

Touchpad gesture bindings are defined with the `gesture` keyword, followed by the gesture specification and command.

Format:
```
gesture <type>[:<fingers>][:<directions>]
	<command>
```

#### Gesture Types

- `swipe` - Multi-finger swipe gesture
- `pinch` - Multi-finger pinch gesture
- `hold` - Multi-finger hold gesture

#### Fingers

- `1` to `5` - Number of fingers
- `*` - Any number of fingers (use `*` literally)

#### Directions

Swipe directions:
- `left`, `right`, `up`, `down`

Pinch directions:
- `inward` - Fingers moving together
- `outward` - Fingers moving apart
- `clockwise` - Fingers rotating clockwise
- `counterclockwise` - Fingers rotating counter-clockwise

Combine multiple directions with `+`, e.g., `left+up`

#### Examples

```
# 3-finger swipe for desktop switching
gesture swipe:3:left
	bmsg desktop -f -l prev
gesture swipe:3:right
	bmsg desktop -f -l next

# 4-finger swipe
gesture swipe:4:up
	bmsg desktop -f -l next
gesture swipe:4:down
	bmsg desktop -f -l prev

# Pinch to toggle fullscreen
gesture pinch:in
	bmsg node --state fullscreen
gesture pinch:out
	bmsg node --state fullscreen

# Hold to show desktop (any fingers)
gesture hold:*
	bmsg desktop -f -l prev
```

### Output Commands

```
bmsg output list
bmsg output <name> enable
bmsg output <name> disable
bmsg output <name> mode <width>x<height>[@<refresh>]
bmsg output <name> position <x> <y>
bmsg output <name> scale <factor>
bmsg output <name> transform <normal|90|180|270|flipped|flipped-90|flipped-270>
bmsg output <name> dpms on|off
bmsg output <name> adaptive_sync on|off
bmsg output <name> render_bit_depth 8|10
bmsg output <name> color_profile gamma22
bmsg output <name> color_profile srgb
bmsg output <name> color_profile icc /path/to/profile.icc
```

- `name` — connector name (e.g. `DP-1`, `HDMI-A-1`)
- `description` — human-readable description string
- `make` / `model` / `serial` — manufacturer, model, and serial number
- `width` / `height` — current resolution in pixels
- `refresh` — current refresh rate in mHz (e.g. `143856` = 143.856 Hz)
- `scale` — current scale factor
- `phys_width` / `phys_height` — physical display size in millimeters
- `enabled` — whether the output is currently active

`color_profile` controls the color transform applied to the output:

- `gamma22` — default gamma 2.2, no transform applied
- `srgb` — apply an sRGB inverse EOTF transfer function (useful for HDR-capable displays running in SDR mode)
- `icc <path>` — load a custom ICC color profile from a file and apply it as a linear-to-display transform

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
bmsg config window_gap [<n>]     	# Get or set window gap
bmsg config single_monocle [true|false]
bmsg config borderless_monocle [true|false]
bmsg config borderless_singleton [true|false]
bmsg config gapless_monocle [true|false]
bmsg config edge_scroller_pointer_focus [true|false]
bmsg config scroller_default_proportion [<value>]
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

Sets the number of blur passes (1-10, default: 1). Higher values create stronger blur but require more GPU processing.

```
bmsg config blur_radius <value>
```

Sets the blur radius (default: 5.0). Higher values create stronger blur but may cause artifacts.

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
