# HALCYON-SCALE -- the display scale: where it comes from, who owns it, how every pixel follows it

Status: DESIGN (2026-09-08, the H-arc under the operator's autonomy grant;
a heritage-aligned recommended design is auto-accepted on Fable 5.x, the
ratification items in section 9 are flagged for the operator). The
mechanism behind `docs/HALCYON-COMPOSITION.md` section 1, which is the
operator's and states the RULE: every size in HALCYON-VISUAL and
HALCYON-COMPOSITION is a logical pixel at a 96 DPI reference; `scale =
output_DPI / 96` snapped to the nearest 0.25; round half up, uniformly;
hairlines `max(1, round(1 x scale))`, bevels `max(2, round(2 x scale))`.
This document does not restate the rule. It says what in THIS tree
produces the scale, who is allowed to change it, how it reaches the two
processes that paint chrome and glyphs, and what the bakes need.

## 1. What the tree does today (verified 2026-09-08; re-verify before building)

- **tapestryd owns the display.** `GET_DISPLAY_INFO` gives the scanout's
  pixel rect (`gpu.rs` `read_display_info`; `mode auto` re-probes it). The
  driver READS the `VIRTIO_GPU_F_EDID` feature bit for its features line
  (`gpu.rs:735`) but does NOT acknowledge it (`want_lo` carries virgl /
  ctxinit / blob only), and there is no `GET_EDID` (0x010a) -- the command
  is illegal on the wire until the feature is negotiated (VIRTIO 1.2
  section 5.7.3). Nothing in the tree knows a physical size.
- **One metrics table, two consumers.** `libhalcyon::theme::METRICS`
  (bevel 2, gap 2, hairline 1, header_h 20, status_h 20, tag_pad_x 6,
  tab_strip_h 5) is read by tapestryd (`pane.rs` carves the tag bar +
  the ring; `server.rs` carves the status strip and paints the bevel /
  hairline) AND by halcyond (chrome.rs, status.rs). The carve and the
  paint agree today because both read one `const`.
- **halcyond's proportional faces rasterize at any size** (fontdue over
  the vendored Plex TTFs: `GlyphSource::glyph(face, px, ch)`); the
  logical sizes are constants scattered across the renderer: layout.rs
  (body 11.5, prompt 10, hdr 17.5 / 14.5 / 12.5, pad_x 12, block_gap 6,
  the table / kv gaps, `OBJ_PAD` 4, `CODE_PAD` 3, the 1.5 / 1.25
  line-height factors), chrome.rs (`NAME_PX` 10.5, `TRAIL_PX` 9.5, `GAP`
  5), status.rs (`STATUS_PX` 10, `PAD` 8, `GAP` 8, `WS_PAD` 7), menu.rs
  (the name size + the island), raster.rs (`MONO_ISLAND_PX` 12,
  `MONO_GRID_PX` 20), `Sheet` (`daylight_sheet()`).
- **Mono is bake-only.** `FACE_MONO` serves the baked Cornucopia atlases
  (`cornucopia::Atlas::for_advance`): advances 6, 7, 8, 9, 10 exist
  (19-47 KB each; cell_h and baseline are font-derived per advance by
  `tools/bake-cornucopia.py`). The Cornucopia TTF is 10.8 MB (an Iosevka
  build) and is NOT embeddable in halcyond; the bakes are the Plan 9
  subfont idiom and stay.
- **The compositor's ctl** prints `display W H` + six more lines; every
  reader (`libtapestry::parse_two`, halcyond's menuset) picks its line by
  prefix and tolerates others, so a new line costs no reader a change.
- **Session verbs are gated** (menu / `tag <id> status` / `role=chrome` /
  `role=status`): the renderer unconditionally, the DECLARED session for
  what it hosts or owns, E_PERM otherwise (the cfg-3 authority class).
- **QEMU** (`virtio-gpu-pci`) advertises `edid=on` by default with `xres`
  / `yres` props; the EDID it generates carries a physical size QEMU
  derives from a nominal DPI, not the host monitor's -- MEASURED by SC-2's
  boot line on 2026-09-08: `edid 325x203 mm for 1280x800 px`, i.e. 100.0
  DPI on both axes, so on every host we have (the mac window is
  host-scaled; thyla-pi under KVM gets the same QEMU EDID) the derived
  scale snaps to 1.0. The boot line prints the millimetres every boot so
  the claim stays a measurement, not a memory.
- **The atlas** (since 1337a218) packs only what a frame PAINTS and has a
  hard cap of 24 x 512-px pages; the visible set at 1.0 is a few pages.

## 2. Prior art

- **Plan 9** has no DPI concept: `draw` fonts are bitmap subfonts baked
  per size, and the user (or `rio`'s `$font`) picks a size per display.
  The bake set IS this idiom; the scale is then "which subfont", and the
  answer belongs to the thing that knows the display.
- **Wayland** (the SOTA for our shape): `wl_output.scale` (integer) and
  `wp_fractional_scale_v1` (units of 1/120) are COMPOSITOR-owned, per
  output, PUSHED to clients as a property; a client renders at the
  preferred scale and the compositor never resamples a client that
  followed it. **Fuchsia Scenic** does the same with `device_pixel_ratio`
  in the view's metrics event. **Windows** (per-monitor DPI awareness v2)
  posts `WM_DPICHANGED`; **macOS** uses an integer backing scale and
  downsamples the rest; **X11** has a client-side global `Xft.dpi`, the
  model that does not fit a per-seat compositor.
- The synthesis: the compositor derives and owns ONE scale per display,
  publishes it, and every painter follows; the mono sizes are subfonts.

## 3. The value

- **`scale` is a percent**: 100, 125, 150, 175, 200 -- the 0.25 steps of
  COMPOSITION section 1 as integers, so the wire and the ctl carry no
  float. v1 clamps to 100..200 (section 8 says why).
- **Derived**: `dpi = px_w / (mm_w / 25.4)`; `scale = round(dpi / 96 x 4)
  / 4`, clamped, then x100. Both axes are computed and the SMALLER
  wins when they disagree beyond one step (a monitor lying about one
  axis must not blow the other up).
- **From the EDID**, negotiated (the driver acks `F_EDID` when offered)
  and read once at boot after `GET_DISPLAY_INFO` and again on `mode
  auto`: `VIRTIO_GPU_CMD_GET_EDID` for scanout 0 -> `virtio_gpu_resp_edid
  { size, edid[1024] }`. The size in millimetres comes from the FIRST
  detailed timing descriptor (bytes 54.., image size = byte 66 | (byte
  68 >> 4) << 8 by byte 67 | (byte 68 & 0xF) << 8), falling back to the
  basic parameters' centimetres (bytes 21, 22) x 10. A missing feature,
  a refused command, a size of 0 or over 2000 mm on either axis, or a
  checksum that fails is a GARBAGE EDID and yields 100 -- fail-safe to
  the 1.0 the whole tree is built and gated at, said once on the boot
  line with the raw millimetres.
- **Overrides, in precedence order**: the `scale <pct>` ctl verb (section
  4) beats everything for the rest of the session; `scale auto`
  re-derives. The user's `/env/HALCYON_SCALE` is not a third source: the
  session compositor reads it at start and WRITES the verb (section 6).
- **The platform's declaration** (SC-5, 2026-09-08): `thylacine.scale=<pct>`
  on the kernel command line, read ONCE at boot from `/hw/chosen/bootargs`
  (the channel joey's opt-outs and aurora's display mode already ride;
  QEMU's `-append`). When present and one of the five values it IS the
  derived scale -- `scale auto` and a `mode` change return to it, not to
  the EDID -- because a declaration outranks a measurement: it exists for
  the display whose EDID cannot say (QEMU's synthetic one claims 100 DPI at
  any size -- the cocoa backend passes no physical size and virtio-gpu has
  no DPI property -- so a 2560x1600 scanout shown 1:1 on a retina panel
  derived 100 and cost four chords per boot) and for the panel whose EDID
  lies. Prior art: plan9.ini's `monitor=`/`vgasize=` (the boot side
  declares the display; the component that owns the decision reads it),
  Linux's `video=` parameter, Fuchsia's board-level `display_pixel_density`
  (a device-tier declaration that outranks the EDID). Not a pool file: the
  pool is baked per image and a display is per boot. Malformed or off the
  table: said once and ignored, the EDID stands (the same fail-soft as a
  garbage EDID). The verb still beats it for the session. `run-vm.sh` emits
  it from `THYLACINE_SCALE=<pct>`; `THYLACINE_HIDPI=1` implies 200 unless
  told otherwise; nothing is emitted otherwise, so every gate at 1.0 is
  untouched. The boot line names the source: `scale 200 (declared)`.

## 4. The authority and the channel

- **tapestryd is the authority.** `Comp.scale: u16` (percent) is set at
  boot from the platform's declaration, else the EDID, and republished on
  every change; the ctl text gains the line `scale <pct>` (after `display
  W H`; every existing reader is prefix-keyed and unaffected).
- **The verb**: `scale <pct>` (one of the five values; anything else
  E_INVAL) and `scale auto`, admitted for the RENDERER unconditionally
  and for the DECLARED session compositor while it hosts (`conn_hosts`,
  the seat-held-while-hosting rule the menu and the status bar already
  carry); E_PERM otherwise -- the seat's scale is the seat's to set,
  never a per-process client's (the cfg-3 class: an unprivileged client
  must not rescale another principal's display). Judged BEFORE any state
  changes, like the other verbs.
- **What a change does, in order**: `Comp.metrics = Metrics::at(scale)`;
  every carve recomputed (`layout.recompute` with the new tag_h /
  ring / status_h / tab strip; `status_rect`); a STRUCTURAL relayout --
  every surface gets its CONFIGURE, every chrome strip its new size, the
  status bar its new height (the one-bar `create` check reads
  `status_h` at the NEW scale; an existing bar of the old height is
  retired with a CLOSE so its owner re-mints -- the H-3d rearm cadence
  covers it); the ctl republished; then the say line (test builds)
  `tapestryd: scale <pct> (<from>: edid|verb|auto; <mm_w>x<mm_h> mm)`.
- **Bounded**: the verb is on the per-pass verb budget like every layout
  verb (`LAYOUT_VERBS_PER_PASS`), so a client cannot thrash the display
  with alternating scales faster than the pass cadence; a scale equal to
  the current one is a no-op (no relayout, no fan).

## 5. The metrics, at both ends

`libhalcyon::theme::Metrics::at(pct: u16) -> Metrics`, the ONE function
both processes call:

    scale(v)     = round_half_up(v x pct / 100)         -- header_h, status_h, gap, tag_pad_x, tab_strip_h
    hairline     = max(1, scale(1))
    bevel        = max(2, scale(2))

`METRICS` stays as the 1.0 table and a test pins `Metrics::at(100) ==
METRICS` so nothing at 1.0 moves. tapestryd holds `Comp.metrics`; halcyond
holds it in the `Sheet` (below). The two agree by construction (the #230
mirror-by-meaning rule: neither carries a literal the other could drift
from). The floor gap (`gaps`) is the tunable inter-pane gap and stays in
LOGICAL pixels, scaled at the carve like the rest.

## 6. halcyond: one place, every size

- **`Sheet.scale: u16`** (percent) plus `Sheet.metrics: Metrics`. Every
  logical size becomes physical through ONE helper, `Sheet::px(&self,
  logical: f32) -> f32` (glyph sizes stay fractional: fontdue takes an
  f32; sub-pixel glyph placement is normal per COMPOSITION section 1) and
  `Sheet::ipx(&self, logical: i32) -> i32` (round half up) for paddings,
  gaps, pills, rules. `daylight_sheet(scale)` builds the sheet; the
  constants named in section 1 are the LOGICAL inputs and stop being used
  as pixels anywhere: layout.rs (body / prompt / hdr / pad_x / block_gap
  / the gaps / OBJ_PAD / CODE_PAD; the line-height FACTORS are unitless
  and unchanged), chrome.rs (NAME_PX / TRAIL_PX / GAP / the pad from
  `metrics.tag_pad_x`), status.rs (STATUS_PX / PAD / GAP / WS_PAD),
  menu.rs (the name size, the island). The hairline under an obj pill
  and the island gutter use `metrics.hairline`.
- **The mono sizes**: the island wants the advance `round_half_up(6 x
  scale)`, the grid `round_half_up(10 x scale)`: at 125% 8 / 13, at 150%
  9 / 15, at 175% 11 / 18, at 200% 12 / 20. `GlyphSource` selects both
  atlases by advance at scale time (`select_mono(scale)`), and
  `MONO_ISLAND_PX` / `MONO_GRID_PX` become the sheet's `mono_island_px`
  / `mono_grid_px` (2 x advance, the bake's 0.5-em rule). The pts
  geometry of a kaua-term tile follows the grid cell (a larger cell,
  fewer columns -- the tile's winsize report is already the cell-derived
  one).
- **The bakes**: `usr/lib/cornucopia/src/atlas-{11,12,13,15,18,20}.bin`
  via `tools/bake-cornucopia.py --advance N` (about 660 KB in all: the
  advance-20 bake is ~190 KB), `cornucopia::ADVANCES` extended,
  `for_advance` serving them; the kernel trusted sink and Aurora keep
  their advance-10 default. Advance 9 exists already (150% island).
- **A scale change at runtime**: halcyond reads `scale` from the ctl on
  every CONFIGURE / LAYOUT it already handles (it re-reads the layout
  there); when it differs: rebuild the Sheet (`gen` bumps, so every
  layout cache re-lays at the new sizes), `GlyphSource::set_scale`
  (selects the mono atlases; `regen()` -- the author's size-change point
  -- and the atlas cap is re-derived, section 7), the height caches
  cleared (heights depend on metrics now), every tile reflowed and its
  pts resized, chrome + status + menu repainted at their new surface
  sizes (the CONFIGUREs deliver those).
- **The user's override**: `/env/HALCYON_SCALE=<pct>` (the `/env` device;
  halcyond already reads `HALCYON_SESSION` and `HALCYON_PALETTE` there)
  -- the session compositor writes `scale <pct>` once at start, after
  `session on`. It is a preference, not a source: the compositor stays
  the authority and the ctl the channel, so the carve and the paint can
  never disagree.
- **A live control**: two chord actions, `ScaleStep(+1)` / `ScaleStep(-1)`
  (one 25% step, clamped) and `ScaleReset` (`scale auto`), on the RUNTIME
  chord table (`chords.rs`; remappable by the gated `chord` verb like the
  rest) with the defaults Super+= (`KEY_EQUAL` 13), Super+- (`KEY_MINUS`
  12) and Super+0 (`KEY_0` 11) -- all three free in the default table;
  the universal zoom keys of browsers and terminals. Chords act in the
  compositor, so they need no admission.

## 7. Bounds and invariants

- **The atlas cap follows the display** (found while designing this: the
  1337a218 cap is a constant, 24 x 512-px pages = 6 MiB, and the painted
  set is bounded by the DISPLAY AREA, which a 4K scanout multiplies by
  eight). The visible glyph area is at most the screen area; shelf
  packing wastes up to about half a page per size and shelf; so the cap
  is `max(MAX_ATLAS_PAGES + ATLAS_PAGE_SLACK, ceil(2 x display_area /
  page_area) + ATLAS_PAGE_SLACK)`, re-derived whenever the display size
  or the scale changes (`GlyphSource::set_display(w, h)`), and a test
  pins that a full screen of the largest heading at 200% on a 1280x800
  display packs under it. The eviction bound between frames scales the
  same way. At 1280x800 the formula gives the constant it replaces.
- **I-32 (the resource floor)**: the bakes are static; the atlas is
  capped as above; a hostile `scale` verb is gated and budgeted; nothing
  else grows with the scale.
- **The cfg-3 authority class**: the seat's scale is set only by the
  renderer or the declared hosting session; every other conn gets E_PERM
  before any state changes; the battery's negative twin sends `scale
  200` and reads `scale 100` back.
- **Format-fuzz**: the EDID is untrusted input from the device -- bounded
  read (1024 bytes), the checksum verified, every field range-checked,
  garbage yields 100 and never a panic or a stall; `scale <pct>` accepts
  exactly the five values.
- **The two painters agree**: `Metrics::at` is the one function; the
  compose gate at 200% measures the tag bar at 40 px in BOTH the
  compositor's `tagbar` record and the strip's painted pixels.
- **Nothing at 1.0 moves**: `Metrics::at(100) == METRICS`, `Sheet::px` at
  100 is the identity on every constant, and every existing pixel gate
  (ls-halcyon, ls-gfx-compose at 1.0, ls-gfx-panes) stays green
  unchanged -- the discrimination that the plumbing is inert at 1.0.

## 8. Out of scope (v1)

- Scales above 200% (a 4K 15-inch panel is 3.0). The clamp says 200 and
  the boot line says what it measured; lifting it is three more bakes
  (island 15 / 18, grid 25 / 30) and the cap formula already scales.
- Mixed-DPI (one output per seat today; COMPOSITION section 7 defers it).
- Live mono rasterization (the 10.8 MB TTF). If a subset TTF is ever cut,
  the mono path becomes the proportional one and the bakes stay for
  Aurora and the kernel sink.
- Resampling a client that ignores the scale (a Warp / GL surface is
  pixel-native by design; a kaua-term tile follows the grid cell).

## 9. Sub-chunks and the audit

- **SC-1 (mechanical)**: the six bakes + `cornucopia::ADVANCES` +
  `for_advance`; a test that every advance's atlas parses and its cell
  is `(N, font-derived h, baseline)`.
- **SC-2 (tapestryd; audit-bearing: the gate, the EDID parse, the
  relayout)**: `F_EDID` ack + `GET_EDID` + the parse + the derivation
  (host-tested on crafted EDIDs: QEMU's, a real 27-inch 4K's, a garbage
  one); `Metrics::at`; `Comp.scale` / `Comp.metrics`; the ctl line; the
  `scale` verb + its gate + budget; the structural relayout; the chord
  actions; the battery probes.
- **SC-3 (halcyond; audit-bearing: the sweep + the atlas cap + the
  regen)**: `Sheet.scale` + the sweep; `GlyphSource::set_scale` /
  `set_display`; the runtime change path; the env override.
- **SC-4 (gates)**: `gfx_compose.py --scale` (every expectation through
  the same round-half-up); a compose leg that chords to 200%, captures,
  and verdicts at 2.0 (the tag bar 40 px, the body 23 px, the island
  cell 12 x 26-ish per the bake's rule); the 1.0 gates unchanged.
- **ONE Fable round over SC-2 + SC-3** (double-distance), batched with the
  chrome-content close's "ROUND 2 FOCUS" (the atlas fix), since both live
  in the render core.

## 10. For the operator (RATIFIED 2026-09-08 -- "you can proceed with HALCYON-SCALE"; built under the autonomy grant)

- Percent on the ctl / verb (`scale 150`), five values, clamp 100..200.
- The chord defaults Super+= / Super+- / Super+0 (free keys; remappable).
- The atlas cap deriving from the display area (a change to the run-44 /
  1337a218 constant's meaning, not its value at 1280x800).
- The failure posture: a garbage or absent EDID is 1.0, said once -- and
  (the audit's F2) a derivation that lands off the table at boot is 1.0
  too, said on the boot line, the same guard the runtime path applies.
- The fan rule (the audit's F3): a scale change fans the redraw CONFIGURE
  to every visible surface and TEV_LAYOUT to the session WHETHER OR NOT
  the geometry moved -- the Direct arm and a lone leaf under a menu carve
  nothing, and a follower re-reads the ctl only on a relayout.
- Added under the same word, veto if wrong (SC-5, after the ratification):
  the platform's declaration `thylacine.scale=<pct>` (section 3) as the
  derived scale's first source, so a HiDPI guest boots at 2.0 without the
  four chords; `scale auto` returns to the declaration, not the EDID.
