# Doors Configuration Documentation

Doors uses two configuration files located in `~/.config/doors/`:

- `doorsrc` - Startup script executed when the compositor starts
- `doorshkrc` - Hotkey bindings configuration

Both files use `doorsctl` to communicate with the running compositor via IPC.

## doorsrc (Startup Config)

This file is executed as a shell script at server startup. Use it to configure
initial settings and set up desktops.

### Global Environment

Set a variable in the Doors process with IPC:

```sh
doorsctl env set XCURSOR_SIZE 24
doorsctl env set QT_QPA_PLATFORM "wayland;xcb"
```

Commands launched later by Doors, including commands from hotkeys, inherit these
values. To remove a variable, use `doorsctl env unset <name>`.

An `export` in `doorsrc` only changes the environment of that shell and commands
launched directly by it; a child process cannot modify the environment of the
already-running Doors parent. If a value is needed by both, do both explicitly:

```sh
export XCURSOR_SIZE=24
doorsctl env set XCURSOR_SIZE "$XCURSOR_SIZE"
```

Environment variables used while Doors initializes must still be set before
launching Doors; IPC is only available after initialization.

### Config Settings

```
doorsctl config border_width <pixels>
```

Sets the border width around windows.

```
doorsctl config window_gap <pixels>
```

Sets the gap between windows.

```
doorsctl config single_monocle true|false
```

When true, monocle layout shows only the focused window regardless of how many
windows are on the desktop.

```
doorsctl config borderless_monocle true|false
```

When true, removes borders in monocle layout.

```
doorsctl config borderless_singleton true|false
```

When true, removes borders for singleton windows.

```
doorsctl config smart_gaps true|false
```

When true, window gaps are disabled when there is only one tiled window on the
desktop.

```
doorsctl config smart_borders true|false
```

When true, window borders are hidden when there is only one tiled window on the
desktop.

```
doorsctl config focus_wrapping true|false
```

When true, directional focus wraps around when there is no window in the target
direction (e.g., focusing west from the leftmost window wraps to the rightmost).

```
doorsctl config focus_on_activate focus|none|smart|urgent
```

Controls what happens when a window requests activation (new window mapping, XDG activation, `_NET_ACTIVE_WINDOW`, foreign toplevel requests).

- **focus** (default): The requesting window receives keyboard focus immediately.
- **none**: Activation only updates visual state (borders, tab highlights) without stealing keyboard focus.
- **smart**: The window receives keyboard focus only if it is on the currently focused desktop. Otherwise, only visual state is updated.
- **urgent**: The window is marked as urgent (visual indicator) instead of receiving keyboard focus.

```
doorsctl config gapless_monocle true|false
```

When true, windows are rearranged to fill gaps when windows are closed.

```
doorsctl config decoration_mode none|tabs|always|csd
```

Controls how window decorations (titlebars, borders) are displayed:

//TODO: rewrite description for none and tabs
- **none**: All windows render without decorations by signaling them to enter fullscreen mode. Tab bars are also hidden. Provides a clean, minimal interface.

- **tabs**: Tab bars are shown in tabbed layouts, but clients are kept in fullscreen mode to hide client-side decorations. No CSD is visible anywhere.

- **always** (default): Decorations are always visible. In tabbed layouts, server-side tab bars replace client decorations. Outside tabbed layouts, clients use their own CSD.

- **csd**: Client-side decorations are always shown, even in tabbed layouts. Tab bars are hidden and every window draws its own decorations.

Clients that do not support `xdg-decoration` keep their own client-side decorations in `none` and `tabs` modes.

```
doorsctl config hide_lone_tab true|false
```

When true, the tab bar is hidden when a tabbed container has only one window.

```
doorsctl config edge_scroller_pointer_focus true|false
```

When true, pointer focus affects scroller behavior at edges.

```
doorsctl config scroller_default_proportion <value>
```

Sets the default proportion for scroller layout (0.1-1.0).

```
doorsctl config scroller_default_proportion_single <value>
```

Sets the default proportion for a single window in scroller layout (0.1-1.0).

```
doorsctl config scroller_proportion_preset [<values>]
```

Sets preset proportions for scroller layout (comma-separated).

```
doorsctl config scroller_focus_center true|false
```

When true, the focused window is centered in the scroller viewport.

```
doorsctl config scroller_prefer_center true|false
```

When true, scroller layout prefers centering the focused window.

```
doorsctl config scroller_prefer_overspread true|false
```

When true, scroller layout prefers spreading windows to fill available space.

```
doorsctl config scroller_ignore_proportion_single true|false
```

When true, proportion settings are ignored for single-window scroller desktops.

```
doorsctl config scroller_structs <n>
```

Sets the number of visible scroller structs (non-negative integer).

```
doorsctl config tab_color_bar_bg "R G B A"
```

Sets tab bar background color for tabbed layout.

```
doorsctl config tab_color_bg "R G B A"
```

Sets inactive tab background color.

```
doorsctl config tab_color_bg_active "R G B A"
```

Sets active tab background color.

```
doorsctl config tab_color_text "R G B A"
```

Sets inactive tab text color.

```
doorsctl config tab_color_text_active "R G B A"
```

Sets active tab text color.

```
doorsctl config tab_color_sep "R G B A"
```

Sets tab separator color.

Each tab color value is four space-separated floats in the range `0.0-1.0` (`R G B A`).

```
doorsctl config text_font "<font description>"
```

Sets the default Pango font description used for compositor-rendered text (for example: `"Sans 10"` or `"JetBrains Mono 11"`).

```
doorsctl config text_height <pixels>
```

Sets the default text height in pixels used for compositor-rendered text.

```
doorsctl config focus_follows_pointer no|yes|always
```

Controls whether keyboard focus follows the pointer cursor.

- **no** (default): Focus never follows the pointer.
- **yes**: Focus follows the pointer when moving over a window.
- **always**: Same as `yes`, but also clears keyboard focus when the pointer moves to the background or over non-window surfaces (e.g., bars, notifications).

```
doorsctl config pointer_follows_focus true|false
```

When true, the pointer is warped to follow window focus changes.

```
doorsctl config click_to_focus true|false
```

When true, clicking a window focuses it.

```
doorsctl config allow_tearing true|false
```

When true, windows may use tearing page flips (screen tearing) for lower latency.
This allows the compositor to bypass vertical sync for eligible windows.
Can be overridden per-window via window rules (see below).
Per-output tearing is configured separately via `doorsctl output <name> tearing on|off`. Default: false.

```
doorsctl config auto_float_dialogs true|false
```

When true, windows that advertise themselves as dialogs via the `xdg-dialog-v1` protocol are automatically floated when they open. This applies to both modal and non-modal dialogs. Window rules with an explicit `state` still take priority. Default: false.

```
doorsctl config minimize_to_scratchpad true|false
```

When true, clicking the minimize button in the toplevel's decorations sends it to the scratchpad.

```
doorsctl config directional_focus_tightness <0-100>
```

Sets how tightly directional focus navigation favors close windows. 0=least strict, 100=most strict.

```
doorsctl config record_history true|false
```

When true, focus history is recorded for cycling through previous windows.

```
doorsctl config mapping_events_count <n>
```

Sets the number of recent mapping events to track (non-negative integer).

```
doorsctl config ignore_ewmh_fullscreen <0-2>
```

Controls how EWMH fullscreen requests are handled: 0=honor, 1=ignore, 2=auto.

```
doorsctl config split_ratio <value>
```

Sets the default split ratio for new containers (0.0-1.0).

```
doorsctl config automatic_scheme longest_side|alternate|spiral
```

Sets the automatic tiling scheme: `longest_side` splits the largest container, `alternate` alternates direction, `spiral` spirals.

```
doorsctl config initial_polarity first_child|second_child
```

Sets the initial insertion side for new windows (first_child=left/top, second_child=right/bottom).

```
doorsctl config top_padding <pixels>
```

Sets the top padding (margin) for all desktops.

```
doorsctl config right_padding <pixels>
```

Sets the right padding for all desktops.

```
doorsctl config bottom_padding <pixels>
```

Sets the bottom padding for all desktops.

```
doorsctl config left_padding <pixels>
```

Sets the left padding for all desktops.

```
doorsctl config normal_border_color <color>
```

Sets the border color for unfocused/inactive windows.

```
doorsctl config active_border_color <color>
```

Sets the border color for the active window within a container.

```
doorsctl config focused_border_color <color>
```

Sets the border color for the focused window.

```
doorsctl config presel_feedback_color <color>
```

Sets the color of the preselection feedback indicator.

```
doorsctl config tiling_drag_indicator_color <color>
```

Sets the color of the tiling drag overlay indicator (default: `4d9eff4d`).

```
doorsctl config normal_border_gradient <colors...> [angledeg]
doorsctl config active_border_gradient <colors...> [angledeg]
doorsctl config focused_border_gradient <colors...> [angledeg]
```

Sets a linear gradient border that replaces the solid border color for the respective state. The value is a space-separated list of hex `RRGGBB` or
`RRGGBBAA` color stops, optionally followed by a rotation angle (e.g. `90deg`). A gradient with at least 2 stops is required; use `clear` to
reset back to the solid border color.

Examples:
```
# Red -> Blue gradient at 45° angle
doorsctl config focused_border_gradient ff0000 0000ff 45deg

# Red -> Blue -> Green gradient at 0° angle
doorsctl config focused_border_gradient ff0000 0000ff 00ff00

# Reset to solid color
doorsctl config focused_border_gradient clear
```

```
doorsctl config normal_border_gradient2 <colors...> [angledeg]
doorsctl config active_border_gradient2 <colors...> [angledeg]
doorsctl config focused_border_gradient2 <colors...> [angledeg]
```

Sets a secondary gradient that blends with the primary gradient via the
corresponding `_lerp` setting. Same format as the primary gradient.

```
doorsctl config normal_border_gradient_lerp <0.0-1.0>
doorsctl config active_border_gradient_lerp <0.0-1.0>
doorsctl config focused_border_gradient_lerp <0.0-1.0>
```

Controls the blend between the primary and secondary gradient. `0.0` shows
only the primary gradient, `1.0` shows only the secondary. Linear
interpolation is applied between the two.

### Output/Desktop Setup

```
doorsctl output <name> desktops <name1> <name2> ...
doorsctl output <name> -d <name1> <name2> ...
```

Configures desktops on the focused output. Desktop names are space-separated.
The number of desktops is determined by the number of names provided.

## doorshkrc (Hotkey Config)

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

When holding Alt with a mouse button on a floating window, doors provides built-in interactive operations:

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

All commands use the `doorsctl` prefix. Commands not starting with `doorsctl` are executed as shell commands.

#### Node/Window Commands

```
doorsctl node --close                  						# Close focused window
doorsctl node --focus                  						# Focus most recently focused window
doorsctl node --state tiled            						# Set focused window to tiled
doorsctl node --state floating         						# Set focused window to floating
doorsctl node --state fullscreen       						# Set focused window to fullscreen
doorsctl node --to-desktop <n>         						# Send window to desktop n (1-10)
doorsctl node --to-desktop <name>      						# Send window to named desktop
doorsctl node --flag hidden=true       						# Toggle hidden flag on window
doorsctl node --flag sticky=on         						# Toggle sticky flag on window
doorsctl node --flag private=off       						# Toggle private flag on window
doorsctl node --flag locked=true       						# Toggle locked flag on window
doorsctl node --flag marked=off        						# Toggle marked flag on window
doorsctl node --flag blur=on|off       						# Toggle blur effect on window
doorsctl node --flag mica=on|off       						# Toggle mica effect on window
doorsctl node --flag acrylic=on|off    						# Toggle acrylic effect on window
doorsctl node --flag border_radius=<float>  			# Set border radius on window
doorsctl node --flag shadow=on|off       					# Toggle shadow effect on window
doorsctl node --flag opacity=<0.0-1.0>  					# Set opacity on window
doorsctl node --move <dx> <dy>         						# Move floating window by delta x,y
doorsctl node --resize <handle> <dx> <dy>  				# Resize floating window (handle: northwest/nw/north/n/ne/east/e/southeast/se/south/s/southwest/sw/west/w/center/c)
doorsctl node --activate               						# Activate focused window
doorsctl node --kill                   						# Kill focused window
doorsctl node --to-monitor <name>      						# Send window to monitor
doorsctl node --to-node <id>           						# Send window to node with id
doorsctl node --layer below|normal|above  				# Set window layer
doorsctl node --type horizontal|vertical|tabbed  	# Set split type for container
doorsctl node --type next_tab|prev_tab            # Navigate tabs in current tabbed container
doorsctl node --ratio <value>          						# Set split ratio for container
doorsctl node --scratchpad             						# Send focused window to scratchpad
doorsctl node --circulate forward|backward  			# Circulate focus in tree
doorsctl node --insert-receptacle      						# Insert receptacle at focused node
doorsctl node --presel-dir west|east|north|south  # Preselect direction for next window
doorsctl node --presel-ratio <value>   						# Set preselection ratio
doorsctl node --swap <id>              						# Swap with node by id
doorsctl node interactive_move         						# Interactive move (for mouse button binds on floating windows)
doorsctl node interactive_resize       						# Interactive resize (for mouse button binds on floating windows)
doorsctl node tiling_drag              						# Tiling drag (for mouse button binds on tiled windows)
```

#### Desktop Commands

```
doorsctl desktop <n>                   # Switch to desktop n (1-10)
doorsctl desktop <name>                # Switch to named desktop
doorsctl desktop next                  # Switch to next desktop
doorsctl desktop prev                  # Switch to previous desktop
doorsctl desktop --focus <name>        # Focus named desktop
doorsctl desktop --focus next          # Focus next desktop
doorsctl desktop --focus prev          # Focus previous desktop
doorsctl desktop --layout tiled        # Toggle tiled layout on desktop
doorsctl desktop --layout tiled --all  # Set tiled layout on every desktop
doorsctl desktop --layout monocle      # Toggle monocle layout on desktop
doorsctl desktop --layout scroller     # Toggle scroller layout on desktop
doorsctl desktop --layout master_stack # Toggle master-stack layout on desktop
doorsctl desktop --rename <newname>    # Rename desktop
doorsctl desktop --swap <name>         # Swap contents with another desktop on same monitor
doorsctl desktop --remove              # Remove current desktop (fails if only desktop)
doorsctl desktop --bubble up|prev|down|next  # Reorder desktop in list
doorsctl desktop --to-monitor <name>   # Move desktop to another monitor
```

#### Focus Commands

```
doorsctl focus west|w                  # Focus window to the left
doorsctl focus east|e                  # Focus window to the right
doorsctl focus north|n                 # Focus window above
doorsctl focus south|s                  # Focus window below
```

#### Swap Commands

```
doorsctl swap west|w                    # Swap with window to the left
doorsctl swap east|e                    # Swap with window to the right
doorsctl swap north|n                   # Swap with window above
doorsctl swap south|s                   # Swap with window below
```

#### Preselection Commands

```
doorsctl presel west|w                  # Preselect left direction for next window
doorsctl presel east|e                   # Preselect right direction for next window
doorsctl presel north|n                  # Preselect above direction for next window
doorsctl presel south|s                  # Preselect below direction for next window
doorsctl presel cancel                   # Cancel current preselection
```

#### Toggle Commands

```
doorsctl toggle floating                # Toggle focused window floating/tiled
doorsctl toggle fullscreen              # Toggle focused window fullscreen
doorsctl toggle pseudo_tiled            # Toggle focused window pseudo-tiled
doorsctl toggle monocle                 # Toggle monocle layout on desktop
doorsctl toggle master_stack            # Toggle master-stack layout on desktop
```

#### Scratchpad Commands

The scratchpad provides a way to temporarily hide windows and bring them back when needed.

```
doorsctl scratchpad show                # Toggle scratchpad (auto): hide focused if scratchpad, or show oldest hidden entry
doorsctl scratchpad show app_id:kitty   # Show a specific scratchpad entry by app_id
doorsctl scratchpad show title:terminal # Show a specific scratchpad entry by title
doorsctl node --scratchpad              # Send focused window to scratchpad
```

#### Rotate/Flip Commands

```
doorsctl rotate clockwise|cw           # Rotate window layout clockwise
doorsctl rotate counterclockwise|ccw   # Rotate window layout counter-clockwise
doorsctl flip horizontal|h             # Flip window layout horizontally
doorsctl flip vertical|v               # Flip window layout vertically
```

#### Send Commands

```
doorsctl send next                      # Send focused window to next desktop
doorsctl send prev|previous             # Send focused window to previous desktop
```

#### Other Commands

```
doorsctl quit                           # Quit the compositor
<shell command>                         # Execute any shell command (no doorsctl prefix)
```

#### Window Rules

Window rules allow you to control how specific applications are managed when they open.

```
doorsctl rule -a <app_id> [options...]  # Add a new rule
doorsctl rule -r <index>                # Remove rule by index
doorsctl rule -l                        # List all rules
```

**Adding Rules (`-a` / `--add`):**
Specify an app_id (or title) followed by consequence options:

```
doorsctl rule -a dev.zed.Zed desktop=I
doorsctl rule -a zen desktop=II
doorsctl rule -a vesktop desktop=III
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
- `block_out_from_screenshare=on|off` - Blocks out the window from screenshare and screenshot tools.
- `allow_tearing=on|off` - Enables or disables tearing for the matched window(s), overriding the global `allow_tearing` config.
- `shortcuts_inhibitor=on|off` - Controls whether the matched window(s) can use the keyboard shortcuts inhibitor protocol to suppress compositor keybindings (default: on).
- `shadow=on|off` - Whether a window should have the shadow effect set
- `render_unfocused=on|off` - When on, the window continues rendering even when hidden/unfocused. Configure the throttle rate via `doorsctl config render_unfocused_fps <value>` (default: 5).
- `opacity=<0.0-1.0>` - Set the opacity of the matched window(s)

**Matching by Title or Tag:**

```
doorsctl rule -a title="Exact Window Title" desktop=I
doorsctl rule -a tag="browser" desktop=II
```

**Examples:**

```
# Send Zed to desktop I, don't switch focus away from current desktop
doorsctl rule -a dev.zed.Zed desktop=I follow=off

# Send Firefox to desktop II as floating window
doorsctl rule -a firefox desktop=II state=floating

# Match by tag (set by client via xdg_toplevel_tag protocol)
doorsctl rule -a tag="important" state=floating

# Ignore a window (don't manage it)
doorsctl rule -a some-app manage=off

# Start a window in fullscreen
doorsctl rule -a mpv state=fullscreen

# List all configured rules
doorsctl rule -l

# Remove rule at index 0
doorsctl rule -r 0
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
	doorsctl desktop -f -l prev
gesture swipe:3:right
	doorsctl desktop -f -l next

# 4-finger swipe
gesture swipe:4:up
	doorsctl desktop -f -l next
gesture swipe:4:down
	doorsctl desktop -f -l prev

# Pinch to toggle fullscreen
gesture pinch:in
	doorsctl node --state fullscreen
gesture pinch:out
	doorsctl node --state fullscreen

# Hold to show desktop (any fingers)
gesture hold:*
	doorsctl desktop -f -l prev
```

### Hot Corner Bindings

Hot corner bindings trigger when the cursor enters a corner region of the focused output. They provide a convenient way to execute commands by moving the mouse to screen corners.

Format:
```
hotcorner topleft|topright|bottomleft|bottomright
	<command>
```

### Output Commands

```
doorsctl output list                                         # List all outputs
doorsctl output create                                        # Create a virtual output
doorsctl output <name> enable                                 # Enable output
doorsctl output <name> disable                                # Disable output
doorsctl output <name> mode <width>x<height>[@<refresh>]      # Set display mode of output
doorsctl output <name> position <x> <y>                       # Set output coordinates in layout space
doorsctl output <name> scale <factor>                         # Set output scale
doorsctl output <name> transform <normal|90|180|270|flipped|flipped-90|flipped-270> # Set rotation
doorsctl output <name> dpms on|off                            # Set dpms state
doorsctl output <name> adaptive_sync on|off                   # Set adaptive sync on/off
doorsctl output <name> render_bit_depth 8|10                  # Set bits per pixel to be rendered on output (if supported by renderer)
doorsctl output <name> color_profile gamma22|srgb             # Set color profile
doorsctl output <name> color_profile icc /path/to/profile.icc # Set icc color profile
doorsctl output <name> hdr on|off                             # Enable/disable HDR on output (requires Vulkan renderer, PQ/BT.2020 display support)
doorsctl output <name> tearing on|off                         # Set output to tear (no buffering)
doorsctl output <name> max_render_time off|<ms>               # Set output max render time
doorsctl output <name> focus                                  # Focus output
doorsctl output <name> rename <newname>                       # Rename output
doorsctl output <name> desktops                               # List desktop names on output
doorsctl output <name> desktops <names...>                    # Reset/replace desktops on output
doorsctl output desktops <names...>                           # Reset desktops on focused output
doorsctl output <name> add-desktops <names...>                # Add desktops to output
doorsctl output <name> swap-desktops <target>                 # Swap desktops with another output
doorsctl output <name> remove                                 # Remove output (must have no desktops)
doorsctl output <name> rectangle <WxH:X,Y>                    # Set output rectangle geometry
doorsctl output <name> reorder-desktops <names...>            # Reorder desktops on output
```

- `name` - connector name (e.g. `DP-1`, `HDMI-A-1`)
- `description` - human-readable description string
- `make` / `model` / `serial` - manufacturer, model, and serial number
- `width` / `height` - current resolution in pixels
- `refresh` - current refresh rate
- `scale` - current scale factor
- `phys_width` / `phys_height` - physical display size in millimeters
- `hdr` enables High Dynamic Range output on the display. Vulkan renderer and PQ/BT.2020 display support are required.
- `enabled` - whether the output is currently active

`color_profile` controls the color transform applied to the output:

- `gamma22` - default gamma 2.2, no transform applied
- `srgb` - apply an sRGB inverse EOTF transfer function (useful for HDR-capable displays running in SDR mode)
- `icc <path>` - load a custom ICC color profile from a file and apply it as a linear-to-display transform

### Seat Commands

Seats represent independent keyboard/pointer/focus contexts. Each seat has its own keyboard focus, pointer focus, clipboard, and input method state. By default a single seat (`seat0`) is created at startup.

```
doorsctl seat list
```

Lists all seats and marks the default seat with `(default)`.

### Input Commands

Target specific devices:
```
doorsctl input <device-name> <property> <value>
doorsctl input type:keyboard <property> <value>
doorsctl input type:pointer <property> <value>
doorsctl input type:touchpad <property> <value>
doorsctl input type:touchscreen <property> <value>
doorsctl input * <property> <value>
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
doorsctl query -T --tree               # Get window tree
doorsctl query -M --monitors           # List monitors
doorsctl query -D --desktops           # List desktops
doorsctl query -N --nodes              # List node IDs
doorsctl query -f --focused            # Get JSON info about focused node
doorsctl query ... -m <name>           # Filter results by monitor
doorsctl query ... -d <name>           # Filter results by desktop
doorsctl query ... -n <id>             # Filter results by node id
doorsctl query ... --names             # Output names instead of IDs
```

### Config Commands

```
doorsctl config border_width [<n>]    # Get or set border width
doorsctl config window_gap [<n>]     	# Get or set window gap
doorsctl config single_monocle [true|false]
doorsctl config borderless_monocle [true|false]
doorsctl config borderless_singleton [true|false]
doorsctl config smart_gaps [true|false]
doorsctl config smart_borders [true|false]
doorsctl config focus_wrapping [true|false]
doorsctl config focus_on_activate focus|none|smart|urgent
doorsctl config gapless_monocle [true|false]
doorsctl config enable_animations [true|false]
doorsctl config edge_scroller_pointer_focus [true|false]
doorsctl config scroller_default_proportion [<value>]
doorsctl config scroller_proportion_preset [<values>]
doorsctl config animation_bezier [<name>]
doorsctl config animation_duration [<ms>]
doorsctl config animation <type> [bezier|duration|spring|enabled] [<value>]
doorsctl config render_unfocused_fps [<n>]   # Get or set render_unfocused FPS throttle (1>=, default 5)
doorsctl config idle_timeout [<s>]         # Get or set idle timeout in seconds (0=disable, default 0)
doorsctl config idle_dpms [true|false]     # Get or set automatic DPMS on idle (default true)
doorsctl config realtime_scheduling [true|false]  # Get or set real-time scheduling (default false)
```

### Animation Settings

Doors supports per-type animation configuration with custom bezier curves.

```
doorsctl config enable_animations true|false
```

Globally enable or disable all animations (default: false).

```
doorsctl bezier <name> <p1x> <p1y> <p2x> <p2y>
```

Register named cubic bezier curves for use as animation easing functions. A cubic bezier is defined by two control points - P0=(0,0) and P3=(1,1) are implicit.

- `<name>` - Name to reference this curve by
- `<p1x> <p1y>` - First control point (x and y, 0.0-1.0)
- `<p2x> <p2y>` - Second control point (x and y, 0.0-1.0)

Built-in curves:

| Name          | Equivalent CSS                     |
|---------------|------------------------------------|
| `default`     | `cubic-bezier(1/3, 1, 2/3, 1)`     |
| `linear`      | `cubic-bezier(0, 0, 1, 1)`         |
| `ease`        | `cubic-bezier(0.25, 0.1, 0.25, 1)` |
| `ease_in`     | `cubic-bezier(0.42, 0, 1, 1)`      |
| `ease_out`    | `cubic-bezier(0, 0, 0.58, 1)`      |
| `ease_in_out` | `cubic-bezier(0.42, 0, 0.58, 1)`   |

#### Global Animation Defaults

```
doorsctl config animation_bezier [<name>]
```

Get or set the global default bezier curve used for all animations when no
per-type override is configured.

```
doorsctl config animation_duration [<ms>]
```

Get or set the global default animation duration in milliseconds (default: 180).

#### Per-Type Animation Configuration

Each animation type can have its own bezier curve and duration, overriding the global defaults. The animation types are:

| Type              | Description                          |
|-------------------|--------------------------------------|
| `geometry`        | Window movement and position changes |
| `resize`          | Snapshot window resize animations    |
| `fade_in`         | Windows appearing (fade in)          |
| `fade_out`        | Windows closing (fade out)           |
| `fade_in_layer`   | Layer surfaces appearing             |
| `fade_out_layer`  | Layer surfaces closing               |
| `workspace_slide` | Workspace switching slide            |


```
doorsctl config animation <type> bezier <name>
```

Set a per-type bezier.

```
doorsctl config animation <type> duration <ms>
```

Set a per-type duration (ms).

```
doorsctl config animation <type>
```

Query the current config for a type.

```
doorsctl config animation <type> bezier ""
```

Clear a per-type override (revert to global default).

```
doorsctl config animation <type> enabled [true|false]
```

Enable or disable a specific animation type independently. When disabled, the animation is skipped entirely.

#### Custom Spring Curves

Spring curves use a damped harmonic oscillator model for physics-based
animations. Unlike bezier curves (which have a fixed duration), springs
naturally settle based on their parameters — they can overshoot, bounce,
and feel more organic.

```
doorsctl spring <name> <stiffness> <damping> [mass] [value_eps] [velocity_eps]
```

- `<name>` - Name to reference this spring by
- `<stiffness>` - Pull-back force (higher = snappier)
- `<damping>` - Energy dissipation (higher = less bounce)
- `[mass]` - Inertia (higher = slower/heavier, default: 1.0)
- `[value_eps]` - Position settle threshold (default: 0.001)
- `[velocity_eps]` - Velocity settle threshold (default: 0.001)

Built-in springs:

| Name       | Stiffness | Damping | Mass | Character              |
|------------|-----------|---------|------|------------------------|
| `default`  | 300       | 20      | 1    | Snappy, no overshoot   |
| `bouncy`   | 400       | 10      | 1    | Fast with bounce       |
| `gentle`   | 100       | 15      | 1    | Soft and smooth        |
| `slow`     | 50        | 10      | 2    | Heavy                  |

Example:

```
doorsctl spring snappy 500 15 1          # Very fast, slight overshoot
doorsctl spring floaty 80 5 3            # Heavy, bouncy
```

#### Per-Type Spring Configuration

When a spring is assigned to an animation type, it takes priority over any
bezier curve - the animation runs until the spring settles, ignoring the
duration entirely.

```
doorsctl config animation <type> spring <name>
```

```
doorsctl config animation <type> spring ""     # Clear spring, revert to bezier
```

### Blur Settings

Doors supports window background blur effects using OpenGL shaders. Three blur algorithms are available: `kawase` (default), `gaussian`, and `box`.

```
doorsctl config blur_enabled true|false
```

Enables or disables the blur effect (default: true).

```
doorsctl config blur_algorithm none|kawase|gaussian|box|refraction|lens_refraction
```

Sets the blur algorithm (default: kawase). `refraction` and `lens_refraction` apply a glass-like distortion effect (see Refraction Settings).

```
doorsctl config blur_passes <n>
```

Sets the number of blur passes (1-10, default: 1). Higher values create stronger blur but require more GPU processing.

```
doorsctl config blur_radius <value>
```

Sets the blur radius (default: 5.0). Higher values create stronger blur but may cause artifacts.

```
doorsctl config blur_downsample 4
```

Sets the blur downsample, can be between 1-8 (default: 4).

```
doorsctl config blur_vibrancy <value>
```

Enhances color saturation in blurred areas (0.0-1.0, default: 0.0). Supported for gaussian and box blur algorithms.
Higher values create more vivid, saturated colors in the blur effect.

```
doorsctl config blur_vibrancy_darkness <value>
```

Controls how much dark/dim colors are boosted by vibrancy (0.0-1.0, default: 0.5). Only applies when vibrancy is enabled.

```
doorsctl config blur_noise_strength <value>
```

Adds noise dithering to reduce banding artifacts in smooth gradients (0.0-1.0, default: 0.0). Supported for kawase blur.
Higher values add more visible noise/grain to reduce banding.

```
doorsctl config blur_brightness <value>
```

Adjusts the brightness of blurred areas (0.5-2.0, default: 1.0).

```
doorsctl config blur_contrast <value>
```

Adjusts the contrast of blurred areas (0.5-2.0, default: 1.0).

### Mica Settings

Mica is a background effect that captures and tints the content behind windows.

```
doorsctl config mica_enabled true|false
```

Enables or disables mica effect (default: false).

```
doorsctl config mica_tint_strength <value>
```

Sets the tint strength for mica effect (0.0-1.0, default: 0.35).

```
doorsctl config mica_tint <R> <G> <B> [A]
```

Sets the tint color for mica effect (default: 0.12 0.12 0.14 1.0). RGBA values should be in the range 0.0-1.0.

### Acrylic Settings

Acrylic is a background effect that applies blur with a tinted overlay and subtle noise texture, simulating frosted glass.

```
doorsctl config acrylic_tint <R> <G> <B> [A]
```

Sets the tint color for acrylic effect (default: 1.0 1.0 1.0 1.0). RGBA values should be in the range 0.0-1.0.

```
doorsctl config acrylic_tint_strength <value>
```

Sets the tint strength for acrylic effect (0.0-1.0, default: 0.3).

```
doorsctl config acrylic_noise_strength <value>
```

Sets the noise/grain strength for acrylic effect (0.0-1.0, default: 0.02).

```
doorsctl config acrylic_light_anchor <A> <B>
```

Sets the light anchor pos, (default: 0.5 0.5). Values should be in range -1.0 to 1.0.

```
doorsctl config acrylic_blur_passes <n>
```

Sets the number of blur passes for acrylic effect (0-10, default: 4). Higher values create stronger blur.

### Shadow Settings

Doors supports per-window drop shadows with configurable size, offset, and color.

```
doorsctl config shadow_size <pixels>
```

Sets the shadow blur radius in pixels (default: 8).

```
doorsctl config shadow_offset_x <pixels>
```

Sets the horizontal shadow offset in pixels (default: 0). Negative values shift left, positive shift right.

```
doorsctl config shadow_offset_y <pixels>
```

Sets the vertical shadow offset in pixels (default: 4). Negative values shift up, positive shift down.

```
doorsctl config shadow_color <R> <G> <B> <A>
```

Sets the shadow color (default: 0 0 0 0.5). RGBA values should be in the range 0.0-1.0.

### Refraction Settings

Refraction creates a glass-like distortion effect on blurred backgrounds, similar to the "Better Blur DX" KWin effect.

```
doorsctl config refraction_strength <0.0-30.0>
```

Sets the refraction distortion strength (default: 8.0).

```
doorsctl config refraction_edge_size_px <0.0-400.0>
```

Sets the edge glow/lighting effect size in pixels (default: 230.0).

```
doorsctl config refraction_corner_radius_px <0.0-400.0>
```

Sets the corner radius for the refraction effect in pixels (default: 80.0).

```
doorsctl config refraction_normal_pow <0.0-8.0>
```

Sets the power/exponent for the refraction normal map (default: 4.0).

```
doorsctl config refraction_rgb_fringing <0.0-1.0>
```

Sets the chromatic aberration (RGB color fringing) strength (default: 0.15).

```
doorsctl config refraction_texture_repeat_mode <0|1>
```

Texture repeat mode: 0=clamp, 1=mirrored repeat (default: 1).

```
doorsctl config refraction_offset <0.0-8.0>
```

Sets the refraction offset value (default: 3.0).

### Screen Shaders

Screen shaders display as a filter above content. GLSL fragment shaders are supported.

```
doorsctl config screen_shader grayscale|invert|sepia|nightlight|none
```

Enable one of the built-in shaders.

```
doorsctl config screen_shader_file <path>
```

Load a screen shader from a file, full path must be specified.

```
doorsctl config screen_shader_enabled true|false
```

Globally set if screen shaders should be enabled.

### WM Commands

```
doorsctl wm --dump-state               # Dump current WM state as JSON
doorsctl wm --load-state               # Load WM state (not implemented)
doorsctl wm --add-monitor <name>       # Add a new monitor
doorsctl wm -g --get-status            # Return compositor status (monitor/desktop/node counts)
doorsctl wm -h [true|false]            # Get or set focus record history
doorsctl wm -o --adopt-orphans         # Adopt orphaned toplevels into tree
doorsctl wm -r --restart               # Restart the compositor
```

### Subscribe Commands

```
doorsctl subscribe [-c <count>] [-f <fifo>] <event>...
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
doorsctl keyboard_grouping none|smart|default
```

Set keyboard grouping mode.

### Scroller Commands

```
doorsctl scroller proportion <value>          # Set proportion for focused scroller client
doorsctl scroller stack                       # Stack focused client with previous
doorsctl scroller unstack                     # Unstack focused client from its stack
doorsctl scroller resize <delta>              # Resize focused scroller client width by delta
doorsctl scroller set_proportion <value>      # Set scroller proportion (0.1-1.0)
doorsctl scroller cycle_preset                # Cycle to next proportion preset
doorsctl scroller center                      # Center focused scroller window in viewport
```

### Master-Stack Commands

Master-stack layout splits the desktop into a master area (containing N windows) and a stack area (containing the remaining windows).

```
doorsctl master_stack cycle_orientation      # Cycle orientation: left, top, right, bottom
doorsctl master_stack cycle_stack_layout     # Toggle stack area between vertical/horizontal
doorsctl master_stack inc                    # Increase the number of master windows
doorsctl master_stack dec                    # Decrease the number of master windows
doorsctl master_stack flip                   # Flip master side (left to right, top to bottom)
doorsctl master_stack set_count <n>          # Set master count to n (1-10)
doorsctl master_stack set_ratio <0.1-0.9>    # Set master/stack split ratio
```

The master area is sized according to the configured ratio (default 0.5). The orientation controls which side the master is on. When only one window is present, it takes the full area regardless of the ratio. Use focus and swap binds (west/east) to move between master and stack.

### Hotkey Commands

Add or list keybinds on the fly without editing `doorshkrc`. Keybinds added this way are active immediately and use the same parsing as the config file.

```
doorsctl hotkey list
```

List all currently registered keybinds. Tab-separated columns: index, modifier combination, key name, and action (built-in name or external shell command).
Submap membership is shown in brackets (e.g. `focus_west [submap: focus]`).

```
doorsctl hotkey <modifiers+key> <command> [args...]
```

Register a new keybind at runtime. The `<modifiers+key>` syntax matches
`doorshkrc` (e.g. `alt+d`, `super+shift+Return`). The command and its
arguments are joined and parsed the same way as the config file - commands
prefixed with `doorsctl` are mapped to built-in actions; anything else is
executed as a shell command.

Examples:

```
doorsctl hotkey "alt+d" doorsctl desktop --focus next
doorsctl hotkey "super+Return" foot
doorsctl hotkey "alt+shift+l" doorsctl toggle floating
doorsctl hotkey "ctrl+alt+t" doorsctl node --state tiled
```

### Equalize/Balance Commands

```
doorsctl equalize # Equalize window sizes in tree
doorsctl balance  # Balance window sizes in tree
```

### Primary selection

Primary selection (middle mouse paste) is enabled by default. To disable it, add the enviroment variable `DOORS_DISABLE_PRIMARY_SELECTION=1` to your environment before doors launches. Do not put this in the `doorsrc` as those commands run after primary selection is set up.
