# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)
- Resize animation still looks buggy

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders
- Toplevels with `blur=on` do not render toplevels with blur or mica behind it
- On intel+vulkan, `mica=on` shows the wrong content in the buffer (potentially a non-updating fullscreen capture instead of background), `acrylic=on` looks strange and `blur=on` shows the toplevels' surface blurred instead of what is behind it blurred.
- Some AMD rendering issues, will fix these after intel ones

# Layout
- Better tab grouping (see sway or Hyprland for reference)
- Undersized toplevels that are tiled will jump in position unexpectedly during resize, and lose their surface clip temporarily if they have one on another axis (observed on protonvpn-app)
- Better scrolling layout handling (see niri for reference)

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
