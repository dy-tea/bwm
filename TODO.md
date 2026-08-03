# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)
- Tiled and interactive resize does not have stable position (edge(s) that should not move do(es) not have stable position)

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders
- Floating toplevels with mica are missing rounded border clip on their surface

# Layout
- Better tab grouping (see sway or Hyprland for reference)

# Misc
- Rework the docs to be easier to use
- Improve the README (include video, images, better info)
- Looks like there is a 1px gap between toplevels and borders under certain conditions, likely a rounding error somewhere

# Potential
- Per desktop rules (e.g. floating, master_stack)
- Per layer-surface rules
- Focus grab protocol
- Overview/Expose mode from niri
- Plugin system
- Move animations to fully shader-based
