# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders
- On Intel, mica renders fine but blur does not render at all
- AMD has the following issues: gles2 non-protocol blur buffers render as black (ext-background-effect-v1 protocol, doesn't happen with mica); vulkan incorrectly clips surfaces with rounded borders, mica and shadow do not render
- When shadow is enabled on a toplevel with rounded borders, the shadow seems to be clipped where the toplevel was clipped by the rounded border (either a layering or rendering issue)
- Acrylic appears to not render at all anymore

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
