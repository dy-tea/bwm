# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders
- Floating toplevels with mica are missing rounded border clip on their surface
- Some layer surfaces with blur and a static position do not update the blur surface correctly (layer surface bar doesn't appear to track a toplevel behind it consistently; moving to an empty workspace from a non-empty workspace leaves behind some stale information in the blur surface)
- Intel works perfectly but AMD has the following issues: gles2 non-protocol blur buffers render as black; vulkan has an abort when closing some toplevels, incorrectly clips surfaces with rounded borders and blur surfaces are missing
- When shadow is enabled on a toplevel with rounded borders, the shadow seems to be clipped where the toplevel was clipped by the rounded border (either a layering or rendering issue)

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
