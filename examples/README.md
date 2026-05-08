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
bmsg config disable_decorations true|false
```

When true, requests all windows to render without decorations (titlebars, borders, etc.)
by signaling them to enter fullscreen mode. This provides a clean, minimal interface while
maintaining normal window management. Similar to dwl (Dynamic Window Leader).

The compositor maintains the decoration-free state across window state changes:
- Fullscreening/unfullscreening windows preserves the decoration-free appearance
- Maximizing/unmaximizing windows maintains hidden decorations
- All other window state transitions keep decorations hidden

New windows created after enabling this setting will have no decorations. Existing windows
may need to be restarted for the change to take effect. Normal tiling, floating, and window
management operations continue to work as expected.

```
bmsg config edge_scroller_pointer_focus true|false
```

When true, pointer focus affects scroller behavior at edges.

```
bmsg config scroller_default_proportion <value>
```

Sets the default proportion for scroller layout (0.1-1.0).

```
bmsg config scroller_default_proportion_single <value>
```

Sets the default proportion for a single window in scroller layout (0.1-1.0).

```
bmsg config scroller_proportion_preset [<values>]
```

Sets preset proportions for scroller layout (comma-separated).

```
bmsg config scroller_focus_center true|false
```

When true, the focused window is centered in the scroller viewport.

```
bmsg config scroller_prefer_center true|false
```

When true, scroller layout prefers centering the focused window.

```
bmsg config scroller_prefer_overspread true|false
```

When true, scroller layout prefers spreading windows to fill available space.

```
bmsg config scroller_ignore_proportion_single true|false
```

When true, proportion settings are ignored for single-window scroller desktops.

```
bmsg config scroller_structs <n>
```

Sets the number of visible scroller structs (non-negative integer).

```
bmsg config tab_color_bar_bg "R G B A"
```

Sets tab bar background color for tabbed layout.

```
bmsg config tab_color_bg "R G B A"
```

Sets inactive tab background color.

```
bmsg config tab_color_bg_active "R G B A"
```

Sets active tab background color.

```
bmsg config tab_color_text "R G B A"
```

Sets inactive tab text color.

```
bmsg config tab_color_text_active "R G B A"
```

Sets active tab text color.

```
bmsg config tab_color_sep "R G B A"
```

Sets tab separator color.

Each tab color value is four space-separated floats in the range `0.0-1.0` (`R G B A`).

```
bmsg config text_font "<font description>"
```

Sets the default Pango font description used for compositor-rendered text (for example: `"Sans 10"` or `"JetBrains Mono 11"`).

```
bmsg config text_height <pixels>
```

Sets the default text height in pixels used for compositor-rendered text.

```
bmsg config focus_follows_pointer true|false
```

When true, focus follows the pointer cursor as it moves between windows.

```
bmsg config pointer_follows_focus true|false
```

When true, the pointer is warped to follow window focus changes.

```
bmsg config click_to_focus true|false
```

When true, clicking a window focuses it.

```
bmsg config directional_focus_tightness <0-100>
```

Sets how tightly directional focus navigation favors close windows. 0=least strict, 100=most strict.

```
bmsg config record_history true|false
```

When true, focus history is recorded for cycling through previous windows.

```
bmsg config mapping_events_count <n>
```

Sets the number of recent mapping events to track (non-negative integer).

```
bmsg config ignore_ewmh_fullscreen <0-2>
```

Controls how EWMH fullscreen requests are handled: 0=honor, 1=ignore, 2=auto.

```
bmsg config split_ratio <value>
```

Sets the default split ratio for new containers (0.0-1.0).

```
bmsg config automatic_scheme longest_side|alternate|spiral
```

Sets the automatic tiling scheme: `longest_side` splits the largest container, `alternate` alternates direction, `spiral` spirals.

```
bmsg config initial_polarity first_child|second_child
```

Sets the initial insertion side for new windows (first_child=left/top, second_child=right/bottom).

```
bmsg config top_padding <pixels>
```

Sets the top padding (margin) for all desktops.

```
bmsg config right_padding <pixels>
```

Sets the right padding for all desktops.

```
bmsg config bottom_padding <pixels>
```

Sets the bottom padding for all desktops.

```
bmsg config left_padding <pixels>
```

Sets the left padding for all desktops.

```
bmsg config normal_border_color <color>
```

Sets the border color for unfocused/inactive windows.

```
bmsg config active_border_color <color>
```

Sets the border color for the active window within a container.

```
bmsg config focused_border_color <color>
```

Sets the border color for the focused window.

```
bmsg config presel_feedback_color <color>
```

Sets the color of the preselection feedback indicator.

### Output/Desktop Setup

```
bmsg output <name> desktops <name1> <name2> ...
bmsg output <name> -d <name1> <name2> ...
```

Configures desktops on the focused output. Desktop names are space-separated.
The number of desktops is determined by the number of names provided.

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
bmsg node --flag blur=on|off       						# Toggle blur effect on window
bmsg node --flag mica=on|off       						# Toggle mica effect on window
bmsg node --flag acrylic=on|off    						# Toggle acrylic effect on window
bmsg node --flag border_radius=<float>  				# Set border radius on window
bmsg node --move <dx> <dy>         						# Move floating window by delta x,y
bmsg node --resize <handle> <dx> <dy>  				# Resize floating window (handle: northwest/nw/north/n/ne/east/e/southeast/se/south/s/southwest/sw/west/w/center/c)
bmsg node --activate               						# Activate focused window
bmsg node --kill                   						# Kill focused window
bmsg node --to-monitor <name>      						# Send window to monitor
bmsg node --to-node <id>           						# Send window to node with id
bmsg node --layer below|normal|above  				# Set window layer
bmsg node --type horizontal|vertical|tabbed  	# Set split type for container
bmsg node --type next_tab|prev_tab            # Navigate tabs in current tabbed container
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
bmsg desktop --layout monocle      # Set desktop to monocle layout
bmsg desktop --layout scroller     # Set desktop to scroller layout
bmsg desktop --rename <newname>    # Rename desktop
bmsg desktop --swap <name>         # Swap contents with another desktop on same monitor
bmsg desktop --remove              # Remove current desktop (fails if only desktop)
bmsg desktop --bubble up|prev|down|next  # Reorder desktop in list
bmsg desktop --to-monitor <name>   # Move desktop to another monitor
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
- `monitor=<name>` - Send window to a specified monitor (e.g. `monitor=HDMI-A-1`, `monitor=eDP-1`)
- `state=<state>` - Set window state: `tiled`, `floating`, `fullscreen`, `pseudo_tiled`
- `follow=on|off` - Whether focus follows the window to its desktop (default: on)
- `focus=on|off` - Whether the window receives focus (default: on)
- `manage=on|off` - Whether the window is managed by the compositor
- `locked=on|off` - Lock window to its desktop
- `hidden=on|off` - Start window hidden
- `sticky=on|off` - Make window sticky (visible on all desktops)
- `one_shot` - Remove rule after first match
- `blur=on|off` - Whether a window should have the blur effect set
- `mica=on|off` - Whether a window should have the mica effect set
- `acrylic=on|off` - Whether a window should have the acrylic effect set
- `border_radius=10.0` - Set the border radius to a given float value
- `scroller_proportion=<0.1-1.0>` - Set the scroller proportion for matched windows
- `scroller_proportion_single=<0.1-1.0>` - Set the single-window scroller proportion for matched windows


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
bmsg output <name> tearing on|off
bmsg output <name> focus                       # Focus output
bmsg output <name> rename <newname>            # Rename output
bmsg output <name> desktops                    # List desktop names on output
bmsg output <name> desktops <names...>         # Reset/replace desktops on output
bmsg output desktops <names...>                # Reset desktops on focused output
bmsg output <name> add-desktops <names...>     # Add desktops to output
bmsg output <name> swap-desktops <target>      # Swap desktops with another output
bmsg output <name> remove                      # Remove output (must have no desktops)
bmsg output <name> rectangle <WxH:X,Y>         # Set output rectangle geometry
bmsg output <name> reorder-desktops <names...> # Reorder desktops on output
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

### Query Commands

```
bmsg query -T --tree               # Get window tree
bmsg query -M --monitors           # List monitors
bmsg query -D --desktops           # List desktops
bmsg query -N --nodes              # List node IDs
bmsg query -f --focused            # Get JSON info about focused node
bmsg query ... -m <name>           # Filter results by monitor
bmsg query ... -d <name>           # Filter results by desktop
bmsg query ... -n <id>             # Filter results by node id
bmsg query ... --names             # Output names instead of IDs
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
bmsg config blur_algorithm none|kawase|gaussian|box|refraction|lens_refraction
```

Sets the blur algorithm (default: kawase). `refraction` and `lens_refraction` apply a glass-like distortion effect (see Refraction Settings).

```
bmsg config blur_passes <n>
```

Sets the number of blur passes (1-10, default: 1). Higher values create stronger blur but require more GPU processing.

```
bmsg config blur_radius <value>
```

Sets the blur radius (default: 5.0). Higher values create stronger blur but may cause artifacts.

```
bmsg config blur_downsample 4
```

Sets the blur downsample, can be between 1-8 (default: 4).

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

### Acrylic Settings

Acrylic is a background effect that applies blur with a tinted overlay and subtle noise texture, simulating frosted glass.

```
bmsg config acrylic_tint <R> <G> <B> [A]
```

Sets the tint color for acrylic effect (default: 1.0 1.0 1.0 1.0). RGBA values should be in the range 0.0-1.0.

```
bmsg config acrylic_tint_strength <value>
```

Sets the tint strength for acrylic effect (0.0-1.0, default: 0.3).

```
bmsg config acrylic_noise_strength <value>
```

Sets the noise/grain strength for acrylic effect (0.0-1.0, default: 0.02).

```
bmsg config acrylic_light_anchor <A> <B>
```

Sets the light anchor pos, (default: 0.5 0.5). Values should be in range -1.0 to 1.0.

```
bmsg config acrylic_blur_passes <n>
```

Sets the number of blur passes for acrylic effect (0-10, default: 4). Higher values create stronger blur.

### Refraction Settings

Refraction creates a glass-like distortion effect on blurred backgrounds, similar to the "Better Blur DX" KWin effect.

```
bmsg config refraction_strength <0.0-30.0>
```

Sets the refraction distortion strength (default: 8.0).

```
bmsg config refraction_edge_size_px <0.0-400.0>
```

Sets the edge glow/lighting effect size in pixels (default: 230.0).

```
bmsg config refraction_corner_radius_px <0.0-400.0>
```

Sets the corner radius for the refraction effect in pixels (default: 80.0).

```
bmsg config refraction_normal_pow <0.0-8.0>
```

Sets the power/exponent for the refraction normal map (default: 4.0).

```
bmsg config refraction_rgb_fringing <0.0-1.0>
```

Sets the chromatic aberration (RGB color fringing) strength (default: 0.15).

```
bmsg config refraction_texture_repeat_mode <0|1>
```

Texture repeat mode: 0=clamp, 1=mirrored repeat (default: 1).

```
bmsg config refraction_offset <0.0-8.0>
```

Sets the refraction offset value (default: 3.0).

### Screen Shaders

Screen shaders display as a filter above content. GLSL fragment shaders are supported.

```
bmsg config screen_shader grayscale|invert|sepia|nightlight|none
```

Enable one of the built-in shaders.

```
bmsg config screen_shader_file <path>
```

Load a screen shader from a file, full path must be specified.

```
bmsg config screen_shader_enabled true|false
```

Globally set if screen shaders should be enabled.

### WM Commands

```
bmsg wm --dump-state               # Dump current WM state as JSON
bmsg wm --load-state               # Load WM state (not implemented)
bmsg wm --add-monitor <name>       # Add a new monitor
bmsg wm -g --get-status            # Return compositor status (monitor/desktop/node counts)
bmsg wm -h [true|false]            # Get or set focus record history
bmsg wm -o --adopt-orphans         # Adopt orphaned toplevels into tree
bmsg wm -r --restart               # Restart the compositor
```

### Subscribe Commands

```
bmsg subscribe [-c <count>] [-f <fifo>] <event>...
```

Subscribe to WM events. Available event types:

```
report|R              # Periodic report output
monitor|M             # All monitor events (add/remove/focus/change)
monitor_add           # Monitor added
monitor_remove        # Monitor removed
monitor_focus         # Monitor focus change
monitor_change        # Monitor property change
desktop|D             # All desktop events (add/remove/focus/change/layout)
desktop_add           # Desktop added
desktop_remove        # Desktop removed
desktop_focus         # Desktop focus change
desktop_change        # Desktop property change
desktop_layout        # Desktop layout change
node|N                # All node events (add/remove/focus/change/state/flag)
node_add              # Node added
node_remove           # Node removed
node_focus            # Node focus change
node_change           # Node property change
node_state            # Node state change
node_flag             # Node flag change
all|A                 # All event types
```

### Keyboard Grouping Commands

```
bmsg keyboard_grouping none|smart|default
```

Set keyboard grouping mode.

### Scroller Commands

```
bmsg scroller proportion <value>          # Set proportion for focused scroller client
bmsg scroller stack                       # Stack focused client with previous
bmsg scroller unstack                     # Unstack focused client from its stack
bmsg scroller resize <delta>              # Resize focused scroller client width by delta
bmsg scroller set_proportion <value>      # Set scroller proportion (0.1-1.0)
bmsg scroller cycle_preset                # Cycle to next proportion preset
bmsg scroller center                      # Center focused scroller window in viewport
```

### Equalize/Balance Commands

```
bmsg equalize # Equalize window sizes in tree
bmsg balance  # Balance window sizes in tree
```
