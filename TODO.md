# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)
- Resize animations are imperfect (tiled resize)

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders

# Layout
- Better tab grouping (see sway or Hyprland for reference)
- Better scrolling layout from niri (not using bsp tree)

# Misc
- Better HDR handling (might have a broken implementation)
- Rework the docs to be easier to use
- Figure out why foot is undersized and if undersized logic is needed
- Minimum sizes are currently always respected, clipping may be preferred in most cases
- Let more than 1 rule match a given client
- Improve the README (include video, images, better info)

# Potential
- Per desktop rules (e.g. floating, master_stack)
- Per layer-surface rules
- Focus grab protocol
- Overview/Expose mode from niri
- Plugin system
- Move animations to fully shader-based
