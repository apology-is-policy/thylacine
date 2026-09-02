# HALCYON.md — the graphical environment

**Binding scripture.** Adopted 2026-09-01 (the Halcyon kickoff design
conversation, user-ratified in-session: the three-source UX vision presented by
the operator, every surfaced fork resolved, the Beacon name chosen). This
document is the authoritative model of **Halcyon, the graphical environment** —
the phase ROADMAP §11 opens. It supersedes the Phase-0 deliverable sketch
(ROADMAP §11.1) and the "Halcyon is pure 2D" posture (ROADMAP §11 EVOLVED note,
TAPESTRY §17), both of which predate the measured Vulkan substrate.

**Where it sits among scripture** (each doc keeps its own authority):

- `docs/TAPESTRY.md` §13–18 — the **compositor** (tapestryd): panes, placement,
  present/resize/event protocol, `/dev/tapestry`. Unchanged and authoritative;
  Halcyon is its client.
- `docs/BEACON.md` — the **semantic output markup** (born alongside this doc).
- `docs/AURORA.md` — the **textual sibling**; the two-environments statement
  (VISION §3.3) stands: Aurora ships regardless, and Halcyon hosts Aurora
  terminals as panes.
- `docs/WARP-WSI-DESIGN.md` — the **measured Vulkan substrate** this phase
  builds on (§8.1 the inverted comparison; §8.2 compose's numbers-condition MET;
  §8.3 the deferred GL-parity ledger).
- `docs/UTOPIA-VISUAL.md` + `docs/COREUTILS-THYLACINE-DESIGN.md` — **Bonfire**,
  the palette / visual language Halcyon's theme complements.

---

## 1. Thesis — three sources, three layers

Halcyon's UX rests on three inspirations (operator, 2026-09-01), and the fusion
is coherent because each lands at a different layer of the stack:

- **i3wm → the compositor layer.** Tiling, not windows: uniform containers with
  `split-h | split-v | tabbed | stacked` modes, and **any tile runs anything** —
  a textual shell, Quake, a video, a graphical Vivarium (Linux-phenotype)
  application. This layer is ratified scripture (TAPESTRY §14) and substantially
  **as-built** (the G-6a/b/c pane tree, chords, focus, resize run in the daily
  boot). D5 placement-transparency is what makes "any tile, any client" true.
- **acme → the chrome layer.** Vertical tab stacking (the Stacked mode already
  renders the Acme shape) and **executable text**: the tag line is a live
  command surface, not a label. Ratified as the recorded Halcyon-era direction
  (TAPESTRY §14); this phase builds it.
- **Symbolics Genera → the pane-content layer.** The genuinely new territory,
  and this document's core: a **rich textual shell render** — proportional and
  monospace type mixed in one transcript, output annotated with meaning
  (Beacon), typed objects with live verbs (presentations). Genera proved the
  model on a Lisp machine; Halcyon rebuilds it on a POSIX-compatible OS with
  graceful two-tier degradation, which no production system has done.

The Phase-0 soul survives intact underneath: a Halcyon shell pane **is** a
scroll buffer whose transcript holds inline graphical surfaces (NOVEL Angle #4,
"preserved and subsumed"). The Genera layer makes that transcript *literate* —
a Dynamic-Listener-class surface rather than a character grid.

## 2. Rendering — Vulkan, one path

**Halcyon renders with Vulkan.** This formally retires "Halcyon is pure 2D /
decode-then-blit" (ROADMAP §11 EVOLVED 2026-06-08; TAPESTRY §17), which was
written as insurance against a GPU stack that did not exist. The stack now
exists, is audited (I-45 staged), and is **measured fastest** on the box (the
W-4 inversion: GL 44.8 / vk-linear 47.6 / vk-blit 51.3 fps on real V3D). The
standing "design for Halcyon-on-vk" directive is hereby cashed:

- **Halcyon is a Vulkan client of tapestryd** exactly as vkQuake is: venus →
  the host GPU on GPU-capable hosts (thyla-pi-class KVM/V3D; the QEMU GL host
  with host-side lavapipe is a certified lane), presenting through the WSI
  DIRECT/composed machinery the Warp arc landed.
- **Text is glyph-atlas rendering** (the alacritty/kitty model, boring and
  proven): glyphs rasterized CPU-side by the native `no_std` TTF rasterizer
  (TAPESTRY §14 — "foundational, not a nicety"; it becomes a deliverable of
  this phase), cached in GPU atlases, drawn as quads. Kerning-pair support:
  yes (DejaVu carries real kern pairs). Complex shaping (HarfBuzz-class
  CJK/ligatures/RTL): deferred, unchanged.
- **Eye-candy has a home, later**: GPU compose puts translucency (the §15 RGBA
  reservation, carried "from day one" for exactly this), animation, and 3D
  within reach. None of it is v1.0 scope; the point of vk is that the door is
  open and the substrate is not a rewrite away.
- **The universal floor (RESOLVED same-day by the §13 concretization pass)**:
  the local macOS dev loop and mode-1 "Thylacine ships with QEMU" deployments
  have **no venus** (the host cannot offer blob+hostmem) — and the answer is
  not a second vk ICD but the **§13.1 architecture itself**: halcyond renders
  through a display list with two executors, and the **CPU executor over the
  proven `libtapestry` weave path IS the universal floor** — Halcyon runs
  wherever aurora runs, GPU host or none, with the vk executor as the
  accelerated path where venus exists. Guest-lavapipe (vk-on-CPU as a second
  ICD, plausible since the Clade arc landed in-guest LLVM JIT) is thereby
  demoted from load-bearing to a post-v1.0 curiosity for CPU-3D
  (ROADMAP §11.9).

## 3. Typography and theme — the two lights, literally

- **Faces**: **Cornucopia** (monospace; the system face — the baked atlas keeps
  serving Aurora/trusted-sink/Halls, the TTF outline serves Halcyon at
  arbitrary sizes) + **DejaVu Sans Condensed** (proportional; operator-chosen;
  free license, vendorable under `third_party/` with a manifest).
- **The default theme is paper-light** — the Genera black-on-white heritage, as
  a **complementary Bonfire schema**: the same palette identity re-grounded on
  light (aurora's held-proposal `parchment` theme is the natural seed). The
  identity this buys is exact: **Aurora is light against the dark — the dawn;
  Halcyon is dark text in full daylight — the calm day.** The palettes
  themselves say which environment you are in. (Dark Halcyon themes remain a
  stylesheet matter, not a design fork.)
- **What stays monospace**: raw-VT panes (§5), code (`em class=code` runs), and
  any content whose alignment is character-grid semantics. Tables do NOT force
  monospace: a Beacon `table` renders as a proportional ruled table in Halcyon
  and as box-drawing in the cells tier — same bytes, two realizations
  (BEACON.md §4).

## 4. The pane-content model — two pane classes

**Class 1 — the rich transcript** (the Genera listener; the default shell
pane):

- A Beacon-aware scroll buffer. ut's `zone` frames give it real structure:
  entries are command blocks (prompt + command + output + exit badge), which is
  the data model for "select a past command, tweak, resubmit" and for
  block-level operations (fold a long output, yank a block, re-run).
- Proportional body text (DejaVu) with monospace islands (Cornucopia) where
  semantics demand; Beacon `table`/`hdr`/`em`/`obj` realized per the
  stylesheet; plain un-annotated output renders in monospace exactly as a
  terminal would — foreign programs lose nothing.
- The **Helix-modal transcript** (TAPESTRY §14) governs keyboard interaction:
  Esc → normal mode, navigate/select/yank anywhere in read-only scrollback,
  `i` jumps to the writable prompt. Selection spans mixed-metric content
  (hit-testing is per-glyph, not per-cell).
- Inline graphical surfaces (an image, a video frame, a game thumbnail) are
  **Tapestry surfaces in the transcript flow** — the recorded `inline-live`
  placement state, with the focus-boundary lifetime rule (TAPESTRY §14)
  unchanged.

**Class 2 — the raw-VT pane** (the compat surface):

- Full-screen TUI programs (vim, htop, a pouch/phenotype app) get a classic
  monospace cell grid — **an Aurora-class pane**. The "Aurora terminals as
  panes" continuity (AURORA.md §7) is realized here: the VT core is shared with
  Aurora's parser, not reimplemented.
- The alt-screen switch is the natural class boundary: a program entering the
  alternate screen converts its pane to raw-VT for the duration; on exit the
  transcript resumes. (The same boundary that already separates app-focused
  from shell-scrolling.)

## 5. Presentations in the UX — click what the text means

Beacon `obj` runs are live in Halcyon:

- **Hover/press** shows the resolved target; **the context menu on an `obj`
  run is the verb list for its type** (BEACON.md §7 owns the verb table and the
  security clause — user-invoked only, resolved-ref display, no authority in
  frames).
- **The executable-text unification**: the transcript's presentations, acme-
  style execution of *selected* text, and the pane **tag line** all dispatch
  through one rules engine. Type a command in a pane's tag and run it; select
  a path anywhere and act on it; click a presented object — one mechanism,
  three doors.
- **Context menus are an amendment to the recorded scope-out** ("no transient
  panels"), and a disciplined one: menus are **ephemeral compositor-summoned
  chrome** — short-lived, dismissed on click-away/Esc, and **applications can
  never create floating surfaces**. Who draws them: the ratified tag-bar
  pattern generalized — *the compositor places the geometry, a renderer paints
  the text* (TAPESTRY §14; D7 survives untouched). Genera itself is the
  license: its right-click menu was the presentation system's front door. The
  anti-window thesis is about persistent, app-owned, overlapping surfaces;
  it stands.

## 6. Mouse

Enabled, secondary (keyboard remains primary — unchanged):

- **Tile focus** (click-to-focus, alongside Super+arrows).
- **Selection** in transcripts and VT panes (per-glyph hit-testing in
  proportional content).
- **Context menus** (§5) — on presentations, on panes (pane verbs: split,
  zoom, pin, close), on the tag strip.
- **Layout drag** (rearrangement) — already recorded in TAPESTRY §14's mouse
  scope.

## 7. Layouts — saved, reloaded, respawned

The layout system is a direct payoff of layout-as-9P (TAPESTRY §15):

- **A saved layout is the serialized layout tree plus each pane's tag** — and
  because the tag is the executable command line (acme), **reload respawns
  panes from their tags** (the i3 `append_layout` + swallow precedent, minus
  the swallow hack: our tags are authoritative).
- **Named layouts** live in the two-tier config pattern (aurora-config
  precedent): `/lib/halcyon/layouts/` (system/device tier) +
  `$home/lib/halcyon/layouts/` (session tier). `halcyon.rc` remains a script
  writing `/dev/tapestry` files; a layout file is data it feeds.
- Geometry-only restore (attach-on-next-launch) is the degenerate case of the
  same format; respawn is the ambition and the default.

## 8. Scope

**In (this phase):** the rich transcript + Beacon rendering; the vk renderer +
glyph-atlas text; the TTF rasterizer; DejaVu vendored; the paper-light theme;
presentations + verbs + context menus; the executable tag bar (titles first,
execution per the rules engine); mouse per §6; layouts per §7; raw-VT panes on
the shared VT core; Aurora-as-panes; compose-under-Halcyon (§10); image display
inline (PNG/JPEG via a decode-to-surface path); the Halcyon-surface audit;
`docs/manual` coverage.

**Deferred (named, not dropped):**

- **The Wayland bridge** — a Wayland-protocol adapter service over tapestryd
  (core + shm/dmabuf + xdg-shell) would open the Linux GUI ecosystem (Wine
  included) to Vivarium tiles. It is its own future arc, comparable in weight
  to netd or the PTY arc; **post-1.0** (operator, 2026-09-01 — Wine was an
  example, not scope). SDL-based Linux apps need no bridge and are carryable
  today.
- **Video playback**: the Phase-0 "custom 9P video-player server" deliverable
  is *re-cut at the chunk*: the compat stack may make a ported or phenotype
  player (mpv-class, vk output) the better v1.0 vehicle than bespoke decode.
  Decide when reached — deliberately, not by inheriting the 2026-02 plan.
- Aurora's cells-tier `table` realization (BEACON.md §10); complex shaping;
  the eye-candy pass (translucent chrome — the RGBA reservation cashes when
  taken); guest-lavapipe (post-v1.0 CPU-3D curiosity — no longer
  load-bearing, §2/§13.1).
- The B/A display-wall mechanisms + the GL-parity ledger + the blit default
  flip — parked behind compose per the ROADMAP §11 addendum (2026-09-01).

**Out (the thesis, unchanged):** overlapping / floating / z-ordered windows;
user-draggable free placement; persistent app-owned overlays (menus are
compositor-only and ephemeral); multi-pane IDE / browser-class apps as targets.

## 9. Security + invariants

- **The renderer obligation (I-27)**: Halcyon inherits Aurora's exact posture —
  fully suspended during a SAK episode; the kernel paints corvus's cells; no
  framebuffer access, no input reads during one (TRUSTED-PATH.md). Nothing in
  the vk path changes this: suspension is enforced below the client.
- **Beacon's security clause** (BEACON.md §7): frames render, they never act.
- **No new §28 invariant at this altitude.** The substrate invariants continue
  to govern: T-1/I-40 (present integrity — the composed/presentable spec arms),
  I-45 (GPU authority bounded by context), I-7/I-32/I-37 where the machinery
  rides them. Compose work (§10) discharges the *existing* recorded spec
  obligations (the PDrained drain lands in the same commit as the first
  compose reader — the standing landmine note).
- Image decode (inline display) is a format-fuzz audit surface (the Phase-0
  §11.4 row survives).

## 10. Compose under Halcyon

Per the operator (2026-09-01, ROADMAP §11 addendum): **the composed present arm
opens under this phase**, with its inputs already banked — the W-3c-2 design
notes, `settype=ok blit=landed` proven on real V3D, the I-40 spec's composed +
presentable classes model-checked behind switches, and §8.2's numbers-condition
MET. Compose is what turns "any tile runs anything" from fullscreen-DIRECT into
real tiling for GPU clients — a windowed vkQuake in a split is the acceptance
image. The parked display-wall work (B/A, GL-parity, blit flip) queues behind
it, in that order, per the addendum.

## 11. Sequencing (RESEQUENCED same-day by the §13 concretization pass)

> The adoption-morning sketch put "the vk pane renderer" at H-2 and compose at
> H-5. The concretization pass (§13) found that ordering inverted a
> dependency: **a vk client in a *pane* requires the composed present arm**
> (only fullscreen surfaces bind DIRECT), while the CPU executor on the
> proven weave path (§13.2) needs nothing new — so the transcript MVP comes
> first on the floor executor, compose comes next (it is tapestryd-side work,
> independent of Halcyon's own code), and the vk executor rides after both.
> Guest-lavapipe drops off the critical path entirely: the CPU executor IS
> the universal floor (§13.1).

- **H-0** — this scripture + BEACON.md + the reconciliation + the §13 pass.
- **H-1 — Beacon foundations** (BEACON.md §12, implementation-grade):
  `SYS_FD_DEVCLASS` → the `beacon` crate (cells realization relocated) → ut
  zones → emitters ls/grep/ps/stat. Lands value with no Halcyon binary.
- **H-2 — halcyond + the transcript MVP on the CPU floor** (§13.2–§13.5):
  the shared-VT-core extraction, the display-list module, the CPU executor
  over `libtapestry`, fontdue+DejaVu glyph pipeline, the paper-light theme,
  zones → blocks. **A usable rich Halcyon shell pane, end to end.**
- **H-3 — presentations**: `obj` rendering, the verbs rules engine, context
  menus (compositor-placed chrome, §13.6), the executable tag bar.
- **H-4 — layouts** (§13.7): save/reload/respawn + `halcyon.rc`.
- **H-5 — compose** (§10): the tapestryd composed-present arm (the W-3c-2
  design + the PDrained same-commit obligation). Unlocks windowed vk clients
  generally — vkQuake-in-a-split is the acceptance image.
- **H-6 — the vk executor** (`halcyon-gpu`, §13.1/§13.2): the pouch-side
  display-list executor over the certified venus link set; per-pane composed
  vk surfaces. Requires H-5. The GL-parity ledger + B/A + the blit flip
  queue behind this per the ROADMAP addendum.
- **H-7 — integration + audit**: Aurora-as-panes polish, image display, the
  video decision, the Halcyon-surface audit round, the manual chapter, the
  ROADMAP §11.2 exit-criteria pass.
- *(unscheduled, no longer load-bearing)* — the guest-lavapipe
  investigation: still interesting for CPU-3D later; the universal floor no
  longer depends on it.

## 12. Naming rationale + status

**Halcyon** (locked since Phase 0) — the calm day; the impossible return. The
light family it anchors: **Aurora** the dawn, **Halcyon** the day, **Bonfire**
the palette, **Beacon** the signal (BEACON.md §11). The paper-light default
(§3) makes the pairing literal.

- **2026-09-01**: scripture adopted — the Halcyon kickoff design conversation
  (operator vision: i3 + acme + Genera; all forks resolved in-session; Beacon
  named). Supersedes the Phase-0 §11.1 deliverables and the pure-2D posture.
  No code. H-1 is the natural opening chunk.
- **2026-09-01 (same day)**: §13 added — the concretization design pass
  (ground-truthed; the process architecture bound; the arc resequenced in
  §11). Implementation-grade for H-1/H-2; interface-grade beyond.

---

## 13. The concretization design pass (2026-09-01)

> Same bar as BEACON.md §12: **a session on a lesser model must be able to
> build H-2 from this section without re-deriving any decision**, and must
> not be able to wander into the two dead-end architectures this pass closed.
> Every "exists" claim is verified against the tree (file:line), not
> recalled. TAPESTRY §18 is this section's precedent and altitude model.

### 13.0 Ground truth (verified 2026-09-01)

| Fact | Where | Consequence |
|---|---|---|
| `libtapestry` is a complete CPU-weave client API: `Surface::fullscreen()/open(w,h)`, `pixels() -> &mut [u32]`, `present(rect)/present_rects/present_hold`, `reweave` + `handle_configure` (the G-6b resize protocol), `poll_event/wait_event`, `surface_ctl/global_ctl`, `display_dims()` | `usr/lib/libtapestry/src/lib.rs:186-742` | The CPU executor's substrate exists whole; aurora is its proven consumer. H-2 writes no transport code. |
| Aurora's `Vt` core is **pure `no_std + alloc`** (imports only `alloc::*`): `Vt::new/resize/feed/set_theme`, `Cell`, `Palette`, themes incl. the light `parchment` (held-proposal name) | `usr/aurora/src/vt.rs:13-15, 49, 125, 163, 224-320, 921` | Extractable to a shared crate verbatim (§13.4). The transcript's inline-SGR subset and the raw-VT pane class both come from this one core. |
| Each new client surface splits into the pane tree automatically (`pane.rs host()`); panes carry `pub tag: String` (`pane.rs:162`) and the four modes (`pane.rs:72-90`); the `layout` file renders `epoch N focused N [zoomed N]` + depth-indented `<id>[*] leaf surface=N\|empty [x,y,w,h][ hidden]` / `<id>[*] <mode> n=N active=N [rect]` rows — **tags and leaf modes are NOT in `render_text` today** | `usr/tapestryd/src/pane.rs:1044-1100` | Halcyon-core opens one surface per transcript pane and the compositor places it. The layout SAVE format needs a tag-bearing extension (§13.7). |
| The composed present arm is **tapestryd + host-side** work (SET_TYPE → EGLImage → the C-3 blit; per-host capability verdict, fail-closed to DIRECT), with the four structural changes and the PDrained same-commit rule already designed | `memory/design_w3c2_compose_arm.md` (verified against server.rs then); WSI-DESIGN §7; the I-40 spec's composed class | Compose (H-5) needs zero Halcyon-side code — it unlocks windowed vk for EVERY client. Hence the §11 resequencing. |
| The certified guest-vk client shape is a **pouch/musl C binary**: the venus link set (`libvulkan_virtio.a` whole-archive, `-u vk_icdGetInstanceProcAddr`), device calls via gipa/gdpa; fullscreen binds DIRECT; windowed requires compose | `tools/build.sh::build_vkquake`, `usr/ports/mesa/README.md` "The venus link set", WSI-DESIGN §7 | The vk executor (H-6) is this shape minus the game. No Rust path into venus exists (no Rust-std-pouch lane — verified; and mesa is C). |
| Native-spawns-ported is doctrine-blessed; native-LINKS-ported is an explicit escalation ("a meaningful new direction") | CLAUDE.md §"Native vs ported userspace programs" | The §13.1 architecture is the only shape that is simultaneously doctrine-clean, vk-capable, and Rust-safe where safety pays. |

### 13.1 The process architecture (BOUND; the fork analysis recorded)

**Halcyon is not one program.** It is a small family with one brain:

- **`halcyond`** (native `libthyla-rs`, `no_std + alloc`) — **the
  environment client, and the only place that thinks.** Owns: per-pane
  transcript state, the Beacon parser, the VT core instances, the fontdue
  glyph rasterizer + atlas cache, the stylesheet/theme, the verbs engine +
  menu content, `halcyon.rc` execution and layout save/restore, and a
  **display list** per pane per frame (§13.2).
- **The CPU executor** (a module INSIDE halcyond, not a process) — executes
  a display list into a `libtapestry::Surface`'s pixel weave and presents
  damage rects. This is the **universal floor**: it runs wherever aurora
  runs, GPU host or none. Its drawing vocabulary is exactly `render.rs`'s
  proven idiom (fill, glyph-blit with fg-over-bg alpha, image blit).
- **`halcyon-gpu`** (pouch/musl C; H-6) — the accelerated executor: a child
  process halcyond SPAWNS, speaking the serialized display-list protocol on
  its stdin plus shared image/atlas memory, executing via the certified
  venus stack into per-pane composed vk surfaces. Crash isolation is real
  and cheap: halcyond owns ALL state, so an executor death is respawn +
  redraw, never data loss.

**Why this and not the alternatives** (each rejected on scripture or
substrate, recorded so no future session relitigates blind):

1. *All-pouch Halcyon (C or C-with-a-little-Rust)* — violates the
   authored-native doctrine, and abandons Rust exactly where the CVE classes
   live (font parse, image decode, escape parsing, scrollback state — the
   NOVEL #4 rationale).
2. *Native halcyond linking the venus archives* — the explicitly
   escalation-bearing "native links ported" direction; would need a
   musl-re-export sysroot for the native target. Strictly worse than
   spawning.
3. *Rust-std-on-pouch* — no such lane exists (verified); porting Rust std to
   the pouch target is its own arc with unknown depth. Named as a future
   simplification, not a dependency.
4. *Drop vk; CPU only* — contradicts the ratified direction and forfeits
   compose-era headroom. The floor-executor design keeps CPU as the floor
   without making it the ceiling.

The two-executor cost is bounded by construction: executors are DUMB (a
dozen ops, no layout, no text knowledge — §13.2); everything that could
drift lives once, in halcyond, above the list.

### 13.2 The display list (v0; in-process API now, wire at H-6)

At H-2 the display list is a Rust type consumed by the in-process CPU
executor — **no serialization exists yet**. H-6 adds the wire encoding
(little-endian, versioned, length-prefixed) without changing the op set.
Binding op vocabulary (v0):

```
Clear   { color }                         // whole-surface ground
Rect    { x, y, w, h, color }             // fills; rules; selection bands; strip segments
Glyphs  { atlas_gen, baseline_x, baseline_y, runs: [(glyph_id, x_advance)] , color }
Image   { blob_id, x, y, w, h }           // decoded raster (blob = halcyond-owned pixel store)
Embed   { surface_ref, x, y, w, h }       // an inline Tapestry surface's place in the flow
                                          //   (CPU executor: reserved rect; the compositor
                                          //   places the actual surface — §13.5)
```

Rules: coordinates are surface-local pixels; the executor never measures
text (glyph positions arrive resolved); `atlas_gen` names an atlas page
generation — the executor blits from the page it was HANDED (in-process: a
slice; H-6: shared memory), so a stale generation is impossible by
construction (pages are append-only within a gen; a full page bumps the
gen). Damage: halcyond computes per-frame damage rects and calls
`present_rects` — the executor does not diff.

**AS-BUILT at H-2b (2026-09-01)**: the crate is `usr/lib/cartoon` — a
tapestry **cartoon** is the full-size design the weaver executes, exactly
this artifact's role (halcyond draws the cartoon; executors weave it into
pixels); the thematic name extends the Tapestry family with real weaving
vocabulary (naming rationale in the crate header). Pure no_std + alloc,
zero deps, host-tested (the vt/beacon pattern). Concretizations against
the sketch above, all within the v0 vocabulary: glyph runs live FLAT in
`Cartoon.runs` (ops carry `start`/`count` — already the H-6 wire shape);
the atlas store carries the glyph table (`GlyphEntry` = page + rect +
FreeType-convention bearing) beside its pages, with a shelf packer
(`AtlasPacker`; page growth never bumps the gen — only the author's
explicit `regen()` eviction does); a stale `atlas_gen` skips the whole op
(fail-safe, like every malformed id); `Image` composites src-over at
native size (the op's w/h are the flow reservation; scaling stays out of
the executor's v0); `Embed` paints nothing (the author lays any
placeholder ground beneath with `Rect`); `execute` takes an optional
`ClipRect` execution bound (clamped regardless — no op can write outside
the buffer).

### 13.3 The transcript model (the data structure; H-2's core)

**Store semantics, derive pixels.** The transcript is a bounded deque of
**blocks** (one per Beacon `zone` cycle: prompt text, command-era bytes,
output, exit mark; un-zoned foreign output coalesces into anonymous
blocks). Each block stores its content as **runs**: plain text, SGR-attr
spans (from the inline VT-subset interpreter, §13.4), Beacon spans
(`em`/`obj`/`table`/`hdr` with their args), and `Embed` anchors. **Pixels
are never stored; layout is a pure function** `layout(block, width_px,
stylesheet_gen) -> line boxes`, cached per block and invalidated by width
or stylesheet change — which makes reflow-on-resize correct by construction
(re-run the function; cursor-anchored scroll like aurora's #55 posture) and
makes the 100k-line budget a content budget: stored bytes + per-block
overhead, with the layout cache LRU-bounded separately, sized against the
< 64 MiB exit criterion. Selection (Helix-modal) addresses (block, run,
byte) — glyph hit-testing maps pointer → that triple through the cached
line boxes.

**AS-BUILT at H-2d-1/d-4 (2026-09-01, `usr/halcyond/src/transcript.rs` +
`select.rs`) — three recorded deviations + one decided question:**

- **Cells, not runs**: a block stores **column-addressed cells**
  (`TCell{ch, style}`) under real line discipline (`\r` overwrite, tab
  stops, BS, EL 0/1/2 — EL never moves the cursor), with per-block
  interned styles + obj tables, because runs don't survive `\r`-rewriting
  writers (progress bars, spinners) — the cell grid is the stable address
  space the VT stream actually mutates. Beacon spans die at block edges;
  the SGR pen persists across them.
- **Selection addresses `(block, item, col)` over CELLS**, not
  `(block, run, byte)`: the run/byte triple names a storage shape that no
  longer exists after an overwrite; tables select per-row, and yank
  re-derives cell text joined by two spaces.
- **Un-zoned output is FOREIGN** (the H-1 F11 idle-delivery question,
  decided here): bytes arriving outside any zone accumulate in a block of
  the third kind, `Foreign` — never folded into the preceding output zone
  nor the next prompt. A zone open/close freezes the current block at the
  boundary; an EMPTY Foreign block leaves no trace (its id is reused; ids
  stay dense + monotonic). One tolerance: an `exit` mark landing in an
  empty Foreign block immediately after an output close is attributed to
  the Output block it completed (the pre-deviation-8 floating order).
- Zones do **not nest** in v0: a zone open inside an open zone freezes the
  previous block (flat block sequence; `command` stays RESERVED).

### 13.4 The shared VT core (the extraction; H-2 opens with it)

- `usr/aurora/src/vt.rs` MOVES to a shared crate (`usr/lib/` sibling of
  kaua/libtapestry; name held — `vt` is acceptable, a thematic name may be
  proposed at the chunk); aurora consumes it unchanged (its `render.rs`
  already touches only the cell grid + palette API). The move is
  behavior-preserving and gated by aurora's existing screendump scenarios
  (`ls-gfx*` — a ZERO-diff bar).
- **AS-BUILT at H-2a (2026-09-01)**: the crate is `usr/lib/vt` (name `vt` —
  the standard term won per the naming discipline's don't-force-it rule; the
  §13.9 held slot is resolved). A git 100%-rename plus a 10-line crate
  header; aurora's four use-sites went `crate::vt::` → `vt::`. The gate ran
  as the full 15-scenario `ls-gfx*` family: ALL PASS (gl + glquake included
  — real clade legs, no skips). Bonus the plan did not name: the module's
  9 parser tests, dormant since birth (aurora is a no_std bin crate with no
  cargo-test lane), are LIVE for the first time —
  `cargo test -p vt --target aarch64-apple-darwin`, the beacon-crate
  pattern. Aurora still carries 9 dormant tests (config 3 / render 1 /
  osd 5); the vault's aurora dossier tracks them.
- Halcyon uses it twice: **(a)** the raw-VT pane class hosts a full `Vt`
  grid per alt-screen program — literally an aurora-class pane, cashing the
  "Aurora terminals as panes" continuity; **(b)** the transcript's inline
  interpreter reuses the SGR/attr machinery on a per-block basis (a
  VT-SUBSET: SGR + line discipline; cursor addressing inside a transcript
  block is treated as foreign-fullscreen intent and flips the pane to the
  raw-VT class — the alt-screen switch is the primary trigger, cursor-park
  heuristics stay out of v1).

### 13.5 Fonts, images, and inline surfaces (H-2/H-7 halves)

- **Rasterizer**: fontdue-class (`no_std + alloc` advertised — VERIFY at
  vendor time by building against the native target before any code depends
  on it; ttf-parser+hand-raster is the named fallback if the claim fails).
  Vendored under `third_party/` with manifest + forage registration, like
  every remote input. All font PARSING stays in halcyond (Rust) — executors
  only ever see finished atlas bitmaps.
- **Metrics mixing rule** (the Genera look without ransom-note lines): the
  transcript line-height is DejaVu's, per stylesheet size; Cornucopia
  islands (code/`em code`/aligned content) set their baseline ON the DejaVu
  baseline and may not stretch the line box; box-drawing glyphs appear only
  in raw-VT panes and cells-tier content, never proportional flow.
- **Images** (H-7): PNG decodes in halcyond — the bound recommendation is
  miniz_oxide (`no_std` inflate) + a hand-rolled defilter/chunk walker
  (PNG's spec surface is small; a bespoke decoder is fuzz-friendlier than a
  ported one and keeps the parse in Rust). JPEG: decide at H-7
  (port-vs-defer); not load-bearing for the exit criteria draft.
- **Inline surfaces**: an `Embed` reserves flow space; the actual pixels are
  a Tapestry surface the compositor places (the inline-live placement,
  TAPESTRY §14). The H-2 MVP may land text-only transcripts first; `Embed`
  arrives with image display.

### 13.6 Chrome + menus (H-3 mechanics)

**CONCRETIZED 2026-09-01 (the H-3 design pass; operator-ratified votes recorded
inline).** The visual identity is `docs/HALCYON-VISUAL.md` (Daylight), ratified
this pass as binding scripture for the H-3 chrome. This section is the
mechanics; Daylight is the look.

**Survey ground truth** (tapestryd, verified 2026-09-01 — so no future session
re-derives it): the pane tree (`pane.rs`) already carries per-leaf `tag: String`
+ `role: Role{Content,Chrome,PinTarget}` + `focusable`, but **`Role` is inert**
(stored + reported on the per-pane `role` file, no painter branches on it; only
`focusable` gates directional focus). Strip geometry exists (`visible_strips()`
-> per-strip rects; `TAB_STRIP_H`). A CPU chrome painter exists
(`paint_chrome`->`paint_borders` [a 1px FLAT frame] + `paint_strips` [solid
colored segments, "glyph-free per D7, never text"]) writing into the composed
screen buffer. A **gated global-ctl** exists (`global_ctl` behind
`peer_is_renderer()`, per-write + fail-closed; `is_ungated_ctl()` = default-DENY
denylist) — the cfg-3 pattern, and the menu-verb hook. Client surface-create has
NO role/placement param (a client picks only WxH / fullscreen; surfaces
auto-host into the focused empty leaf). **Absent (H-3 builds):** any chrome
text, bevels beyond 1px, a status bar, menus, input-grab/redirect,
click-away/Esc dismiss, and any behavior keyed on `Role::Chrome`.

**The load-bearing rule** (§5, D7): *the compositor places geometry; a renderer
paints the text.* Applications never create floating surfaces.

**RATIFIED VOTE 1 — the strip painter: halcyond paints the WHOLE strip.**
halcyond paints the entire tag strip OPAQUE (Daylight bg + name/rule/pills/trail
text + the sage/cinnabar live-tile status key, §4 anatomy) into a `Role::Chrome`
surface; the compositor PLACES it at the strip rect (`visible_strips`) and draws
the PANE-level bevel/hairline/cast-shadow around the pane. Opaque = no alpha
overlay compositing. All of Daylight's strip anatomy lives in one place
(halcyond's `Sheet`); the compositor's Daylight knowledge is only the four bevel
constants + the per-tile status-key COLOR it is told. *Alternative B rejected*
(compositor paints bg + status key, halcyond paints text into an alpha overlay):
needs a new alpha-composite path and splits Daylight's strip look across two
painters, for no gain.

**The chrome-surface path (new; H-3b).** `Role::Chrome` is activated: a chrome
surface is one halcyond creates and paints but the compositor PLACES at a strip
rect (not auto-hosted, not focusable, excluded from the scanout-Direct count).
New surface-create plumbing carries a role + a placement binding (a strip id, or
a pane id + strip index). halcyond learns strip rects by reading the pane 9P
tree (the existing per-pane geometry files; no new read verb — the §13.7
file-walk bias). The pane-level status-colored hairline + cast shadow (§5.3/§5.4)
are compositor-drawn and need the tile's status key, so halcyond signals it via a
small gated ctl verb (`tag <pane-id> status ok|err|resting` — name provisional;
rides the `peer_is_renderer` default-deny gate).

**RATIFIED (operator vote 2026-09-01): the tag bar is PER-LEAF, not
per-container.** The "strip rect" above is a **per-leaf tag-bar strip**, NOT the
existing `visible_strips()` per-container tab indicator (that stays the
tab/stack indicator, glyph-free, unchanged). Every visible LEAF contributes its
own 20px Daylight tag bar (HALCYON-VISUAL §3.2/§4; the acme per-window tag line
and i3's per-window title bar are the prior art, both per-window). Concretely:
- **Geometry (H-3b, another content-rect reshape after H-3a-2's ring).**
  `pane.rs recompute` carves `TAG_BAR_H` (= `METRICS.header_h` = 20) off the TOP
  of each visible leaf's inner rect (inside the H-3a-2 floor+bevel+hairline
  ring): from the leaf edge inward, floor + bevel(2) + hairline(1) + **tag bar
  (20)** + client content. The carve is gated the same as the ring (>1 visible
  leaf); a single fullscreen leaf stays borderless AND tag-bar-free (the stage-0
  look preserved; a lone-console tag bar is a separate later decision, a
  deliberate deviation from §3.2's "every tile"). The client-visible content
  rect shrinks by 20px at the top on multi-leaf — a consumer sweep (the
  tapestry-battery geometry + the ls-gfx family) rides this chunk.
- **The strip rect is exposed on the pane 9P tree** (a new per-pane `tagbar`
  geometry file "x y w h", beside `geometry`), and `TAB_STRIP_H` moves into
  `libhalcyon::theme::METRICS` as `tab_strip_h` (the H-3a precedent — the
  single token source). AS-BUILT (H-3b-1): there is NO second `TAG_BAR_H`
  token — `METRICS.header_h` already IS the tag-bar height ("tag bar height
  (20)"), and one value gets one name; the carve reads `header_h` directly.
  So halcyond sizes/places without hardcoding a private
  compositor constant (the survey found `TAB_STRIP_H` was tapestryd-private and
  the strip rect not fully client-derivable — no child-count on the wire).
- **surface-create carries the role + binding via the ctl `create` verb**
  (`create W H role=chrome bind=<pane-id>`) — a ctl-verb TEXT-format change on
  the surface ctl string, NOT a 9P wire/mount break (`libtapestry`
  `open_on` + `surface_ctl`'s `create ` parse, which today rejects a 3rd token).
  `role=chrome` makes the surface non-auto-hosted, non-focusable, and excluded
  from the scanout-Direct leaf count; the compositor places it at `bind`'s
  per-leaf tag-bar strip. AS-BUILT (H-3b-2): `role=chrome` is RENDERER-GATED at
  create (`peer_is_renderer` → E_PERM; syntax errors are E_INVAL for every peer
  and are judged first) — an ungated chrome role would let any client overlay
  fake chrome on another client's pane, so creation joins the gated-verb class
  (the cfg-3 default-deny); the H-3b-4 round prosecutes it. The bind must name
  a live LEAF (E_NOENT, checked before the weave allocation). Placement is one
  authority, `surface_target`: a hosted surface → its pane's content rect, a
  chrome surface → its bound pane's `tagbar` strip (crop, never letterbox;
  invisible while the strip is ZERO or the pane hidden/closed — ids are never
  reused). Chrome rides the structural CONFIGURE fan with the STRIP size (the
  relayout hook) and the frame fan; the compositor's resting `header` fill is
  painted on STRUCTURAL repaints only, so a focus-only repaint never paints
  over a chrome surface's pixels. AS-BUILT (H-3b-3): a FOCUS-only epoch also
  fans the visible chrome surfaces a same-size CONFIGURE (the redraw request,
  coalesced by replacement) — the "resting, active tile" separator moves with
  focus, and halcyond re-reads the layout's `*` on it. halcyond names its own
  pane through the pane's `tag` file ("halcyon"); every other tile shows its
  `tag` or nothing.
- **Aurora (no halcyond chrome surface) on multi-leaf:** the compositor paints
  the per-leaf tag-bar strip with the Daylight `header` background as a resting
  fallback (the pane's `tag` file supplies a name if set) — vote-1-compatible
  because halcyond's OPAQUE chrome surface fully covers the fallback when
  present; aurora just gets a clean resting bar rather than a bare strip.
- **The status verb** sets a per-pane status enum (`ok`->sage / `err`->cinnabar
  / `resting`); `paint_borders` reads it to draw the §5.3 status-colored content
  hairline and the §5.4 cast-shadow's dark companion — **completing the two-tone
  shadow H-3a-2 landed the light half of.** halcyond must be spawned with
  `T_SPAWN_PERM_CONSOLE_RENDERER` (the aurora precedent) for the gated verb to be
  permitted. Pills are DISPLAY-only in H-3b; executing them is H-3c.
  AS-BUILT (H-3b-4): `tag <pane-id> status ok|err|resting` (the provisional
  name kept) records the exit of the tile's LAST completed command -- the one
  fact only the renderer holds (the transcript's `exit` mark) -- as
  `Pane.status`: `resting` = nothing recorded (the default; reset whenever the
  tile's program changes: alloc, host into an empty leaf, the root collapse).
  The compositor keeps FOCUS as its own fact and combines the two at paint time
  (§1.4's key is a function of *(live, last exit)*): the LIVE tile -- the
  focused leaf -- shows sage unless `err` (cinnabar); a tile that is not live
  shows NO key. So a stale or wedged renderer can never leave a status on a
  tile that lost input (the wedged-client robustness the H-3c gate is about,
  applied here), and a focus move re-keys the hairline atomically with the
  compositor's own focus-only repaint -- no second verb, no split brain.
  Syntax first (E_INVAL), then the id must name a live LEAF (E_NOENT; the
  create-bind precedent); the verb rides the cfg-3 default-deny gate, which
  judges authority BEFORE syntax (a non-renderer sees E_PERM whatever it
  writes). Read back via a new per-pane `status` file (`resting|ok|err` -- the
  RECORD, not the display key; ungated like every pane read, the §13.7
  file-walk bias; the H-3d condition slot's source). The hairline (§5.3): the
  live tile's inner hairline is re-keyed alongside the CONTENT -- left, right,
  and the bottom row, which is the §5.4 shadow's dark half above H-3a-2's
  lighter `border` row (the pane's bevel between them is the pane's and stays
  uniform, §2.1); alongside the TAG BAR (the top row + the strip's flanks) it
  takes the bar's TINT, so it still vanishes into the bar as §2.4 intends --
  the bar is tinted, the content is outlined; a bar-less live tile is outlined
  on all four sides. halcyond's strip renders the §4.2 rows: the LIVE tile
  sage/cinnabar (tint ground, key separator, the key's `fg` name); every other
  leaf is a resting pane's sole tile and renders "Resting, active tile" (header
  ground, `ember_deep` separator, `fg` name) -- the plain Resting row belongs
  to a stack's collapsed tiles, which do not exist before tile stacking lands.
  halcyond READS the key from the pane's `status` file (the same record the
  hairline reads -- one authority) and WRITES its own tile's exit through the
  verb on its console surface's conn after every completed command (display
  only: a refusal is said once and the feed stops). The halcyond lib/bin split
  (§13.1) is enforced by the host-test build: the pure rules
  (`halcyond::chrome`: the layout/rect parsers, the key derivation, the strip
  display list) are host-tested; the surfaces + fds live in the bin's
  `chromeset`.
  AS-BUILT (the H-3b audit close; ratified under the standing authorization):
  **the pane tree's trust model.** The per-pane files (`ctl`, `mode`, `tag`,
  `role`) and the `layout` file were ungated since G-6 -- a session-scoped
  layout API any client could drive. Once the tag bar renders a pane's `tag`
  as *the tile's program* and the live key follows focus, that became a hole:
  any client could `close` the console's leaf (halcyond exits on TEV_CLOSE),
  steal focus (keystrokes routed to its tile -- the login prompt on the
  graphical console included), flood a peer with FOCUS records until the
  compositor wedge-retired it, or forge a tile's name. The model now is
  rio's: **a client drives its own window; the environment drives the
  rest.** The console renderer (`peer_is_renderer`, per write) may act on any
  pane. Any other PROCESS may MUTATE a subtree (`split`, `move`, `close`,
  `mode`, `tab`) only if every hosted surface in it is its own (empty leaves
  belong to nobody and never block; an all-empty subtree is anyone's), and
  may TAKE or NAME a tile (`focus`, `zoom`, `focusdir`'s destination, `tag`,
  `role`) only if that leaf hosts its own surface -- an empty leaf is nobody's
  to focus (keystrokes to nowhere) or to name. E_PERM otherwise; every read
  stays ungated (§13.7). The owner is the PROCESS, keyed on the kernel's
  per-Proc `stripes` tag stamped on the conn's peer (monotonic, never reused,
  0 = unknown = owns nothing) -- not the conn: a client holds one session per
  Surface plus a driver session, and the first cut keyed on the conn refused
  the battery's own pane through its driver session. The battery's positive
  control ("focus on OUR pane succeeds", one variable away from its three
  refusals) is what caught it. The compositor's own chord layer acts as the
  environment. Two defences ride beside it: FOCUS coalesces by replacement in
  the event queue (focus is a state, like CONFIGURE), and one conn lands at
  most four layout mutations per service pass (E_AGAIN beyond -- each is a
  full repaint). **The chrome pool.** H-3b-3 minted each tag bar on a session
  of its own against global pools of eight conns and eight surfaces, so a
  third window filled both and every further mint became a five-second
  blocking connect inside the renderer's loop. Now a tag bar is minted on the
  renderer's pane-tree session (`Surface::chrome_on_shared`), the renderer
  peer's per-conn surface cap is widened by `MAX_PANES`, the global pool is
  sized so every conn can reach its cap at once, the listener stays armed
  when the conn pool is full and refuses at once (accept + close, never a
  blocking handshake), and a chrome surface whose bound pane closed is told
  (TEV_CLOSE) and unbound rather than silently orphaned.

**Pane chrome (H-3a).** Extend `paint_borders`/`paint_strips` from the flat 1px
frame to Daylight §2: the NNW single-light-source 2px four-value bevel
(top/left/right/bottom stored as the light direction + derived, never per-edge),
the 1px inner hairline (§2.4), and the live-tile cast shadow (§5.4, owned by the
live tile — never the neighbour's border). Pure compositor geometry, no text —
the first Daylight landing. The `libhalcyon::theme` crate (the Daylight token
source the scripture names) is factored here and shared by the transcript `Sheet`
AND the chrome; the H-2 transcript palette (`parchment_sheet()` from
`vt::THEMES[1]`) is swapped for the Daylight §1 tokens in the same chunk, so
transcript and chrome match from the start.

**Menus (H-3c — THE GATE).** ONE ephemeral `Role::Chrome` surface, summoned by
halcyond via the gated global-ctl verb `menu place <x> <y> <w> <h>` /
`menu dismiss` (names provisional; the default-deny gate). Compositor-placed at
the pointer; **input redirected to halcyond's menu surface while open** (new — no
grab exists today); **dismissed BY THE COMPOSITOR on click-away/Esc** (new — the
compositor, not halcyond, tears it down, so a WEDGED halcyond cannot strand a
modal). Content + verb dispatch are halcyond's (BEACON.md §7's plumber-style
two-tier rules file, `type`->verbs; the security clause is binding — the menu
always displays the RESOLVED ref, frames carry no authority). The
input-redirect + compositor-owned dismiss are the security-critical properties;
the H-3 exit gate is "menu-dismiss-by-compositor proven vs a wedged client."
AS-BUILT (H-3c, 2026-09-02; ratified under the 2026-09-01 standing
authorization): **the menu is a `Role::Menu` SURFACE** (`create W H
role=menu` -- renderer-gated at create like chrome, no bind: the compositor
places it), never hosted, invisible until placed. The gated global verbs are
`menu place <surface-id> <x> <y>` (display coords, clamped into the display;
the surface must be a menu surface owned by the CALLER'S PROCESS -- E_NOENT
if it is no menu, E_PERM if another process's; ONE menu at a time, a second
placement dismisses the first) and `menu dismiss` (the owner's). **The
grab**: while placed, every key routes to the menu surface (Esc is the
compositor's -- its press dismisses and is swallowed, release + repeats
through the chord swallow-set), every pointer event routes to it
menu-relative, a BUTTON press outside its rect is the click-away (dismiss;
the press AND its release swallowed -- rio: a click outside a menu cancels,
never acts), a Super chord dismisses first and then acts, and FOCUS is
untouched (the leaf keeps logical focus under the grab). **Compositor-owned
dismiss = force-retire of the menu surface**, and `retire` carries the
unplace + the heal, so Esc, click-away, a chord, the owner's verb, the
owner's ctl `destroy`, the owner conn's death, a WEDGE and a replacement all
converge on one mechanism that needs nothing from the owner -- the
wedged-client property comes free of it. A placed menu forces Composed and
composes LAST (`menu_reassert` re-composes its last-presented slot over any
screen write that lands under it, on both compose paths); a dismiss returns
to Direct through the F16 pending switch. The heal under a dismissed menu is
TARGETED, not structural (a structural repaint blanks every pane until each
re-presents -- a whole-screen flash per menu): the compositor's own pixels
inside the rect are repainted + pushed (rings, strips, the resting tag-bar
fill, the floor under an empty leaf) and every surface whose target
intersects the rect gets the same-size CONFIGURE, the redraw request every
reveal already heals by. Rio's save-under was REJECTED: on the GPU composed
path the screen buffer holds no client pixels, and both paths must behave
identically from outside (GPU-DESIGN 4.5.9). **Click-to-focus** (§6) landed
with it: a button press in a hosted, focusable leaf that is not the focused
one focuses it, and the press still reaches the client (i3 passes the click
through). **halcyond**: `beacon::verbs` is the ONE rules engine (BEACON.md §7
as-built); the system tier is baked at `/lib/beacon/verbs`; the session tier
is deferred to the settings-channel push (halcyond runs pre-login as the
device's renderer and has no `$home`). Esc-normal: `w`/`b` step obj runs (a
run = the cells of a row sharing one obj index; the index is minted per
frame, so it IS the run's identity), the selected run is underlined in
ember, Enter opens its menu under the run; a left click on a run's glyphs
(hit-tested against the last frame's laid geometry) opens the same menu at
the pointer. The menu: `raised` ground, `border` stroke, the TYPE
(proportional, muted) + the RESOLVED ref (monospace, full ink) first -- the
anti-clickjack line -- a rule, then the verbs in monospace with the selected
one on a `header` band; Up/Down/j/k move, Enter runs (the owner dismisses
through the verb, then the expanded command + newline is typed into the
console -- the tag line's "executes typed text"; the gesture is the choice);
Esc never reaches halcyond. **THE GATE PROOF** (ls-halcyon on the lever): a
`wedge-test` rule the lever bake puts first (`#wedge 6000`, an INTERNAL
action only a test-mode halcyond admits -- the #880 strip class) freezes
halcyond for 6 s with the menu up; Esc dismisses it compositor-side (its
line) while the owner is frozen, keys typed over the real keyboard during
the freeze route to the CONSOLE and run when it wakes (`pwd`'s output-only
path), the rect heals, and no surface is WEDGED. Beside it the H-3c survey
found the H-3b close's shared-session chrome leaking a server-side surface
per dropped tag bar (a `Drop` that closed fds without `destroy`; the
compositor retires only on `destroy` / conn teardown / a wedge) -- fixed in
libtapestry's `Drop` and witnessed by the zoom's `surfaces 1` census. Three
more mechanics the lever forced, all as-built: (1) Esc drains the console
mirror BEFORE it freezes the cursor row, so "everything printed so far" is
what Normal mode sees (keys are serviced before the drain within a pass; a
key sent right after a command's output froze the view one command behind);
(2) the menu is placed FIRST and then painted + presented ONCE -- the weave
slots rotate per present, so a bare second present shows the next slot's
zeros (a black menu); (3) **a 9P session's replies are read only by a thread
inside a wait or an RPC on that session** (ARCHITECTURE 8.8.1.1's elected
reader): the menu -- and every chrome tile -- lives on halcyond's pane-tree
session while the console's stream lives on its own private session, so a
loop parked on the console's ring never sees a menu key (and a tile's
CONFIGURE landed only at the next reconcile's reads). While a menu is up
halcyond waits on the MENU's ring (its FRAME ticks bound the wait; the
compositor's dismiss EOFs it) and polls the console's stream; the chrome
tiles keep their reconcile-driven repaint.

**Status bar (H-3d).** A screen-bottom `Role::Chrome` surface halcyond paints
(Daylight §6): workspaces / focused context / the sage-cinnabar condition slot /
clock. The dark bar that grounds the composition.

**obj interaction (H-3c).** Keyboard-first (§6 makes the mouse secondary):
Esc-normal -> select an obj run -> a key opens its verb menu. Click-to-focus +
click-a-path added in the same chunk if cheap (the survey confirms neither
exists today; pointer routing is under-the-pointer, no click-to-focus).
AS-BUILT (H-3c): all four landed -- `w`/`b`/Enter on obj runs, click-a-path,
click-to-focus (the Menus paragraph above has the mechanics).
AS-BUILT (the H-3c audit close, 2026-09-02): **a release or a repeat follows
its press** -- the compositor records where every key and button press went
(slot + generation) and routes its release or repeat there: across a grab
that began after the press (no stuck key in the leaf), to a dismissed menu's
retired slot (dropped: no stray release in the leaf), and, for a click-away's
press, to the compositor itself, so BOTH edges of a click-away are swallowed
(the first cut set the swallow record and let the retire arm clear it -- the
round's F1: the release reached the pane under the pointer, where a
release-activated widget acts). A menu taller than the display is capped at
it and its list SCROLLS to the selection, by the keys and by the wheel (F3;
wheel deltas to a menu sum in its queue, F4); a placed menu with nothing
hosted under it still forces Composed (F2). The structural repaint pre-fills
every visible pane from its client's last-presented slot, so a menu opened on
a Direct console -- and every split -- no longer blinks the pane blank; the
heal under a dismissed menu also refills the floor around a letterboxed or
cropped surface, which its client can never repaint. A chosen verb runs as
its OWN command line: halcyond feeds ^E ^U before it (ut's editor moves a
half-typed draft to the kill buffer; ^Y restores it), and the system-tier
templates whose programs take `--` carry it (BEACON.md 7).
AS-BUILT (H-3c-2, the event set, 2026-09-02): a renderer's surfaces --
the console, every tag-bar tile, the menu -- live on ONE `tapestry::
EventRing` (one 9P session + one Loom ring; io_uring's one ring per
thread), so one blocking wait wakes for any of their events and one
session's reader demuxes all of them. Two sessions under one thread
starved whichever the thread was not waiting on (a Loom wait pumps one
session): a tile's CONFIGURE landed only at the next pane-tree RPC -- a
focus move between two non-console panes re-keyed no tag bar -- and a
menu's key never, which H-3c had worked around for the menu alone. The
lever's 3-leaf leg witnesses the fix.

**RATIFIED VOTE 2 — scope + sequencing: four sub-chunks, full Daylight.**
H-3a (pane bevels + hairline + cast shadow + the `libhalcyon::theme` crate +
the transcript-palette adoption) -> H-3b (the executable tag bar: the
chrome-surface path + `Role::Chrome` activation + the `tag status` verb) ->
H-3c (obj + verbs + menus — the gate) -> H-3d (the status bar). Each is a
visible, testable landing; the whole Daylight chrome lands across H-3.
*Alternative (roadmap-literal: tag bar + menus + obj/verbs only, defer bevels +
status bar) not chosen* — the transcript would sit in un-beveled panes until a
later chunk.

**Audit-trigger** (ROADMAP §11.1 + AUDIT-TRIGGERS): the H-3b/c ctl verbs
(`tag status`, `menu place/dismiss`) are gated-ctl surfaces on
`usr/tapestryd/src/server.rs` — the cfg-3 default-deny pattern + compositor-owned
dismiss; audit at each of H-3b/H-3c's close. H-3a (geometric painting, no new
authority) is not audit-bearing on its own.

### 13.7 Layouts (H-4; the exact format)

- **Save** = the pane tree serialized with what `render_text`
  (`pane.rs:1044`) prints today PLUS the two fields it omits: each leaf's
  `tag` and each container's full mode/active. Format: a versioned header
  (`halcyon-layout v1`), then the depth-indented rows extended with
  `tag="<escaped>"`. The read side is a new gated tapestryd ctl surface OR
  a halcyond-side walk of `/dev/tapestry` `pane/` files — DECIDE AT THE
  CHUNK; bias to the file-walk (layout-as-9P purity; no new server verb
  for reading what files already expose).
- **Restore** = build the container skeleton via existing pane ctl verbs,
  then respawn each leaf **from its tag** (the tag IS the command line —
  acme; i3's `append_layout` precedent, minus the swallow hack). A leaf
  with an empty tag restores as an empty pane. Geometry-only restore is the
  degenerate case (skip the spawns).
- Named layouts: `/lib/halcyon/layouts/` (device tier) +
  `$home/lib/halcyon/layouts/` (session tier) — the aurora-config two-tier
  precedent, including its hard-won durability discipline (fsync the same
  OWRITE fd post-rename; `gfx-status.md` cfg-2a records the three-iteration
  lesson — do not relearn it).

### 13.8 Audit + scripture-sync obligations (the §18.10 pattern; owed at
### each chunk's close)

- **H-1**: BEACON.md §12.9's list (the syscall row; the cons-verb addendum;
  the spec as-built addendum).
- **H-2**: the VT-core extraction is behavior-preserving (zero-diff
  screendump gate); halcyond's Beacon parser inherits the P3 robustness
  corpus + joins the format-fuzz audit class; a `docs/reference` section
  (or vault dossier per the owner check) for halcyond's architecture.
- **H-3**: the menu/chrome compositor verbs are gated-ctl surfaces (the
  cfg-3 SA-1 default-deny pattern); the verbs rules file is user-authority
  parsing — self-audit the no-execution-without-gesture clause.
- **H-5**: the composed arm's OWN obligations stand unchanged (the PDrained
  drain in the same commit as the first compose reader; the I-40 composed
  cfgs move clean→enforced; the AUDIT-TRIGGERS row extension).
- **H-6**: `halcyon-gpu` joins the venus/WSI audit family (I-45 client-side
  discipline; the display-list wire is a new parse surface on the pouch
  side — bounds-check like the 9P wire).
- **H-7**: the phase audit round (image decode format-fuzz; the transcript
  state machine; the §11.2 exit-criteria pass).

### 13.9 Open items (named; none blocks H-1/H-2's start)

1. fontdue's `no_std` claim — verify at vendor time (§13.5's fallback named).
2. The VT crate name; the halcyond binary name (thematic proposals at the
   chunk, per the naming discipline).
3. The menu ctl verb grammar (H-3; provisional names above).
4. The layout-restore read side (file-walk vs verb; bias recorded).
5. JPEG's vehicle (H-7).
6. BEACON.md §12.10's items (OSC number, env name, devpipe dc, grep ref
   form).
