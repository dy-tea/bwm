# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)
- Resize animation still looks buggy

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders
- Rule-based blur for `blur=on` and `acrylic=on` do not render
- Some AMD rendering issues
- When shadow is enabled on a toplevel with rounded borders, the shadow seems to be clipped where the toplevel was clipped by the rounded border (either a layering or rendering issue)

# Layout
- Better tab grouping (see sway or Hyprland for reference)
- Undersized toplevels that are tiled will jump in position unexpectedly during resize, and lose their surface clip temporarily if they have one on another axis (observed on protonvpn-app)

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
