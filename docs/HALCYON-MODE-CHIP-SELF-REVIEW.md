# Modal status chip self-review — October 5

Single-agent review under the operator's standing direction; not an independent
adversarial audit. Scope: modeview, status/rail painters, statusset repaint and
diagnostic keys, and focused-session state projection. No input ownership,
transport, clipboard authority or kernel mechanism changes.

- Unknown application state is APP. The producer uses the focused leaf's
  existing Normal mode and selection anchor only; it does not infer Nora/ut
  state from title, process name or alternate screen.
- Focus restoration preserves each tile's existing state. Menus do not create
  a competing mode. Existing fullscreen/chrome and trusted-scene policy remain.
- INS/moss, NOR/accent, VIS/dusk and CMD/sand reuse Nora's exported roles.
  APP uses muted neutral fill, opaque. Black/white brightness choice improves
  contrast but is not a measured accessibility guarantee for arbitrary themes.
- All five labels share a maximum-width slot. Both painters use their existing
  typography/scale. Long notices/host names yield room. Display widths below
  the supported range are clipped; no new minimum-display support is claimed.
- The legacy repaint key includes mode. The diagnostic key includes mode but
  excludes content-dependent chip position, avoiding a new log on each exit
  label width. The existing Lantern row/history assertions pass with the new
  transition-only diagnostics.
- Runtime tests exercise APP/NOR/VIS and focus restoration. Host tests exercise
  all five labels, palette roles, scaled raster output and long context. No
  test result is claimed for the unconnected application report bridge.

Measured:558 host tests, native CI build and CPU1 boot1830/1830; isolated mode
scenario49.22s and full Lantern93.78s. Inspected real1280x800 NOR/VIS/focus
captures. Mode paths and screenshots are recorded in the interaction status.
Remaining: application-report integration/read-only flags, proportional text
caret/navigation and completed clipboard. Parked kernel work remains parked.
