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
  `$home/lib/halcyon/layouts/` (session tier). `halcyon.rc` is the user's
  session startup script (§13.7, H-4c): a ut script the per-user compositor
  runs at session start (rio's `-i initcmd`), driving the compositor through
  the session tool (`halcyon layout restore <name>`) -- never through the
  shared `/dev/tapestry` mount, whose peer is the mounter (the H-4b finding).
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

> **REVISED by §14 (multi-console pass, 2026-09-03, operator-ratified):** under the
> multi-console model `halcyond` is the compositor + transcript-orchestrator but is
> **no longer the VT host** — each tile is an isolated `kaua-term` process that
> parses the pts and feeds `halcyond` cells (§14.2/§14.3). "Owns … the VT core
> instances" below is superseded for per-tile terminals; the rest of §13.1 (the
> brain owns the transcript/render/layout; the in-process executor; `halcyon-gpu`
> as a spawned child) stands — and in fact *motivates* the isolation, since a
> hostile-input parser panicking (`no_std` `panic = abort`) would otherwise take
> the whole environment down (§14.2).

**Halcyon is not one program.** It is a small family with one brain:

- **`halcyond`** (native `libthyla-rs`, `no_std + alloc`) — **the
  environment client, and the only place that thinks.** Owns: per-pane
  transcript state, the Beacon parser, the VT core instances, the fontdue
  glyph rasterizer + atlas cache, the stylesheet/theme, the verbs engine +
  menu content, the layout **format** + the save/restore **gesture** + the
  device-tier (pre-login) geometry-only restore, and a **display list** per
  pane per frame (§13.2). The **session-tier** layout save/restore is NOT
  halcyond's: halcyond is a pre-login SYSTEM process with no `$home`, and a
  user's layout lives in the user's session namespace, so it is carried by a
  user-authority session tool (§13.7 H-4, the D decision, ratified 2026-09-02).
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

> **REVISED by §14 (2026-09-03):** the generic-VT parse (`t.feed()`, below) moves
> **out of process** to the per-tile `kaua-term`; the transcript **consumes cells**
> over the seam (§14.3). The `TCell` model and everything above the parse are
> unchanged — the move is a clean refactor precisely because storage is already
> cell-addressed.

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

> **REVISED by §14 (2026-09-03):** §13.4(a)'s raw-VT pane — a full `Vt` grid hosted
> **in `halcyond`** — is superseded by the per-tile `kaua-term` process (§14.2). The
> shared `usr/lib/vt` crate is **grown to full-xterm** and consumed by the
> `kaua-term`; the growth is a cross-tree coordination point (the crate is absent in
> the aux worktree — §14.10).

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

  AS-DESIGNED (H-4, the D decision, ratified 2026-09-02, operator present;
  the pane-tree trust model's SESSION axis). Layout save/restore needs two
  authorities no single component had: renderer authority to build a saved
  SKELETON, and the USER's identity to spawn the tags. halcyond is the
  pre-login SYSTEM renderer (joey-spawned, cross-login, I-27) and ut is a
  console program with no pane connection, so neither can be the restorer.
  D keys ordinary pane authority on the kernel-stamped PRINCIPAL, not the
  per-process `stripes`: a THIRD actor, `Session(principal)`, for a
  non-renderer, non-SYSTEM peer, which may MUTATE a subtree / TAKE-or-NAME a
  tile when every hosted surface in scope shares its principal (the H-3b
  `stripes` rule, broadened by one axis). THREE cases: the console renderer
  (`peer_is_renderer`) is the environment and acts anywhere; a
  SYSTEM-but-not-renderer peer stays `Client(stripes)` (the boot chain must
  not become a session); an ordinary user peer is `Session(principal)`. The
  RATIFIED consequence: same-principal peers gain rio-style mutual pane
  authority -- a program running as you may close/refocus/rename your OTHER
  tiles. This is DELIBERATE and consistent policy: it is strictly weaker than
  the same-owner process kill I-26 already grants you, the console stays
  protected (it is the SYSTEM principal, not yours), and another user's tiles
  stay protected (another principal). The key is the kernel's
  `srv_peer_info.principal_id` -- already stamped, resolved per authority
  write like `peer_is_renderer`, fail-closed for a dead peer, and used by
  tapestryd NOWHERE today -- so the authority needs NO new syscall, `CAP_*`,
  or `SPAWN_PERM_*`: it is tapestryd reading a field the kernel already hands
  it. Empty leaves record an `owner_principal` at split (0 = the renderer's)
  and are reaped when the principal's last live conn is gone; anyone may still
  close an empty (today's rule). PLACEMENT is itself a capability:
  `create ... claim=<tok>` against a one-shot random token minted by reading a
  session-owned empty leaf's `pane/<id>/claim` (the client passes an opaque
  cookie and never observes placement -- the Wayland `xdg_activation` /
  Fuchsia `ViewCreationToken` shape). This REALIZES TAPESTRY.md's per-client
  layout-control capability (task #42) and AURORA-CONFIG's session-leader
  admission, both of which named it. AUDIT: no new section-28 number at v1.0
  (the I-27 "generalizes" precedent) -- a prose obligation under the tapestryd
  pane-tree-trust AUDIT-TRIGGERS row + a focused audit at the H-4b close; a
  candidate invariant is RESERVED for the multi-session future (a non-renderer
  actor's pane mutations never degrade a tile hosting ANOTHER principal's
  surface). The one v1.x seam: focus is a single seat, so a session focusing
  its own empty leaf pulls input from another session's tile; the token is the
  console OWNER's principal (a `srv_peer_info` flag), deferred -- v1.0 has one
  session.

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

RATIFIED (H-3d design, 2026-09-02; the operator present -- two votes below):
- **The placement (the tag-bar precedent, at the display level).** `create W
  H role=status` mints the bar: RENDERER-GATED at create (E_PERM otherwise,
  the cfg-3 default-deny), ONE per renderer (a second is E_INVAL), no bind
  (its bind IS the display), W == the display width and H == the one
  vertical unit (`METRICS.header_h`, 20 -- Daylight 8: "tag bar and status
  bar are both 20px"), else E_INVAL: the bar is never cropped or letterboxed.
  `surface_target`'s status arm places it at `{0, disp_h - header_h, disp_w,
  header_h}`; while a status surface exists the layout is recomputed on
  `disp_h - header_h` (the carve -- the ring, the gaps, every leaf and tag
  bar move up; its retire restores the full display; both are structural
  relayouts and fan CONFIGUREs); a display resize offers the bar a CONFIGURE
  at the new width. The compositor paints the strip `status_bg` from the
  carve on (the resting fill, `paint_borders`), so the bar is dark before
  and between the renderer's presents. The global file `statusbar` reads
  `x y w h` (zeros when none) -- the compositor's fact, the witness's source.
  Never hosted, never focusable, no pointer (chrome by construction). The
  alternatives -- a hosted bottom LEAF (Daylight 6: "the one piece of chrome
  that belongs to the system rather than to any pane"), or halcyond
  painting the bar into the console's own bottom rows (gone the moment the
  console is not fullscreen) -- were not taken.
- **The four slots (Daylight 6), their sources.** *Workspaces*: **ONE filled
  `ember` indicator ("1") until H-4's layouts supply the list -- VOTE
  (2026-09-02): "one filled indicator" over an empty slot or pulling the
  workspace model forward.** *Focused context*: the focused leaf's tag name
  (`pane/<id>/tag`; "transcript" for the console) `·` its working directory
  `·` its last command -- the directory and the command are known only for
  the console (the transcript's own session), so another program's focused
  pane shows its name alone. *Condition*: the focused pane's `status` file,
  the SAME record the live tile keys (sage for ok, cinnabar for err,
  `status_idle` for resting) -- the bar is the redundant channel, the tile
  the primary. *Clock*: `HH:MM` off `CLOCK_REALTIME` (`t_clock_gettime`),
  repainted at the minute (the compositor's FRAME tick is the wake; the
  minute boundary is checked per pass). Proportional face for names,
  monospace islands for the directory and the command (Daylight 7).
- **The working directory comes from ut, as OSC 7 -- VOTE (2026-09-02; an
  ABI addition, signed off): "OSC 7 from ut now" over "program + last command
  only" or "parse the prompt".** ut emits the de-facto standard cwd report
  (`ESC ] 7 ; file://localhost<cwd> ST`) at every prompt, inside the prompt
  zone; the transcript's own escape scanner recognizes OSC 7 and records
  the session's current directory (the latest report). The last command
  comes from ut too, as `mark k=cmd;text=<line>` -- the output zone's FIRST
  child (the exit mark is its last), the registry's one v1 amendment
  (BEACON.md 12.2, the growth policy) -- so the bar shows the RUNNING
  command while the zone is open and the last one after. Aurora keeps
  ignoring both. BEACON.md 12.11 is OSC 7's wire record; it is not a Beacon
  op -- the one foreign OSC the sinks interpret.
- **halcyond paints the bar on its ONE ring** (`Surface::status_on`, the
  H-3c-2 event set): a `status` lib module (the four-slot cartoon list; host
  tests for the layout arithmetic and the truncation order -- the context
  slot yields first, from the left) + a `statusset` bin module (the surface,
  the repaint triggers: a relayout or focus change [the pane-tree read], a
  cwd/command change, a status change, the minute).
- **Witness** (the lever): the `statusbar` rect read off the compositor;
  the strip dark (`status_bg`) at both ends; the ember indicator at the
  left; the condition slot cinnabar after a failing command and sage after
  a passing one, in step with the live tile; the clock slot non-empty; the
  context slot changing after `cd`; the carve measured (the console's
  winsize loses rows; the tag bars move up by 20).
- **Audit-bearing**: `role=status` is a gated-ctl surface on
  `usr/tapestryd/src/server.rs` + a display-level carve (every surface's
  geometry moves); the round at H-3d's close carries the H-3c-2 ROUND 2
  FOCUS (the doubled-cadence rule).

AS-BUILT (H-3d, 2026-09-02): as ratified, with three details the build
fixed. The unit is the theme's own `status_h` (20, == `header_h`; Daylight
8's one vertical unit), so a display shorter than 21 rows or a bar of any
other size is refused. With the bar registered a single leaf is smaller
than the display, so the console is never Direct-scanned while halcyond
runs -- it composes, as it does behind a placed menu and in every split:
the price of a bar that belongs to the system. The context slot truncates
at its RIGHT with an ellipsis (the name proportional, the directory and the
command monospace islands); the workspace indicator is the one ember box
with its number in the bar's own dark; the condition is an 8px dot in the
key colour (sage / cinnabar / `status_idle`) with its word; the clock is
UTC (the RTC's zone; no zone database yet). halcyond re-derives the model
every pass and paints only on a change (the pane tree's focus and status
via the chrome reconcile, the transcript's directory and command, the
minute). The lever's leg: the compositor's `statusbar` rect equals the
bottom strip of ctl's display; both ends `status_bg`; the ember indicator;
the condition dot cinnabar after `cat /nonexistent` and sage after a
passing command, at the slot the `painted` line names; the context shows
the directory after `cd` and the last command.

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

AS-BUILT (the H-3c-2 audit close, 2026-09-02; ratified under the 2026-09-01
standing authorization, the operator back for the close): the ring's slot
machinery moved into a syscall-free `ring` module with host tests, and the
round's findings hardened it -- an errored read ends its stream as EOF does
(a dead compositor no longer livelocks every client), the registered table
is index-stable (a queued-but-unconsumed read can never be re-bound to
another surface's fid), the ring stops at 48 surfaces (the session's 64-tag
table minus the synchronous RPCs' share), a surface nobody polls stops being
read for at 256 queued events (the compositor's own cap then retires it), a
refused `create` says `destroy` (the mint already took a server-side slot),
and the compositor retires a minted-never-created surface whose last ctl fid
clunks. halcyond acts on a one-shot key (Enter, p, v) on its press only --
a held Enter no longer re-summons the menu at the autorepeat rate. OWNED,
deferred to a kernel seam: the held-feed path never demuxes (a timed Loom
enter is the honest primitive; `memory/bug_held_feed_path_never_demuxes.md`).

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
(`tag status`, `menu place/dismiss`) and H-3d's `role=status` create form are
gated-ctl surfaces on `usr/tapestryd/src/server.rs` — the cfg-3 default-deny
pattern + compositor-owned dismiss; audit at each of H-3b/H-3c/H-3d's close. H-3a (geometric painting, no new
authority) is not audit-bearing on its own.

### 13.7 Layouts (H-4; the exact format)

- **Save** = the pane tree serialized with what `render_text`
  (`pane.rs:1044`) prints today PLUS the two fields it omits: each leaf's
  `tag` and each container's full mode/active. Format: a versioned header
  (`halcyon-layout v1`), then the depth-indented rows extended with
  `tag="<escaped>"`. **Read side DECIDED (H-4, the D
  decision): the file-walk** of `/dev/tapestry` `pane/` files (layout-as-9P
  purity; no new server verb). The design pass confirmed the walk needs no
  server addition: `render_pane` (pane.rs) already prints each container's
  `mode n= active=` and every node's rect; only the per-leaf `tag` is absent
  from the `layout` file, and `pane/<id>/tag` already exposes it. (The earlier
  "omits each container's full mode/active" was stale -- `render_pane` prints
  it.) **SAVE is carried by the user-authority session tool for the SESSION
  tier** (the user's `$home` is unreachable to the pre-login SYSTEM halcyond,
  section 13.1); the DEVICE tier (`/lib/halcyon/layouts/`) halcyond may
  write/read directly.
- **Restore** = the **session tool** (`halcyon layout restore <name>`, native,
  coreutil-class), run **as the user** by the user's shell (the Plan 9
  `riostart` / acme `Dump`/`Load` idiom): it reads the layout (session tier
  first, then device), builds the container skeleton via the existing pane ctl
  verbs (an all-empty subtree is anyone's -- section 13.6 -- so the tool needs
  no renderer role for the skeleton), and for each leaf with a non-empty tag
  **claims** the target empty leaf (a one-shot placement token, below) and
  **spawns the tag as the user** (its own identity; the tag IS the command
  line -- acme; i3 `append_layout`, minus the swallow hack). An empty tag
  restores an empty pane. Geometry-only restore is the degenerate case (skip
  the spawns) and is ALSO the device-tier pre-login restore (halcyond,
  renderer authority, no user, no spawns).
- **Placement + naming** is the real gap (`host()` auto-splits into the
  FOCUSED leaf, so it cannot target an arbitrary saved leaf): the tool reads
  `pane/<id>/claim` for a session-owned empty leaf, tapestryd mints a one-shot
  random token, and `create ... claim=<tok>` hosts into that leaf iff still
  empty (else falls back to focus placement). The token rides the tool's own
  `/env` (`TAPESTRY_CLAIM`) into the spawned child (the login
  `seed_session_env` idiom; the client passes an opaque cookie and still
  cannot observe placement -- the Wayland `xdg_activation` / Fuchsia
  `ViewCreationToken` precedent). The tool writes the leaf's `tag` at claim
  time (acme's tag-before-`win`), so a saved layout's tags survive even though
  no ported client names its own leaf today.
- **The shipped device-tier default layout IS the first-launch welcome**
  (ratified 2026-09-02, operator present): a two-pane split, both Utopia on
  Daylight -- LEFT a live, self-demonstrating Genera-style tour (a "try this"
  table whose rows are RUNNABLE objs targeting the RIGHT pane; a clickable
  path obj raising the verb menu), RIGHT a live shell prompt. The pitch is
  SHOWN, not told: the user's first `ls` renders as clickable file objects,
  and the left tour's runnable objs drive the right pane so the
  dead-terminal-vs-live-transcript contrast sells itself. The Genera lineage
  (the Lisp Machine's live presentations, on a Plan 9 shell) is the honest
  narrative. Full UX: `scratchpad/h4-intro-welcome.md`; builds as an H-4 late
  chunk once the format + the tool land.
- Named layouts: `/lib/halcyon/layouts/` (device tier) +
  `$home/lib/halcyon/layouts/` (session tier) — the aurora-config two-tier
  precedent, including its hard-won durability discipline (fsync the same
  OWRITE fd post-rename; `gfx-status.md` cfg-2a records the three-iteration
  lesson — do not relearn it). **H-4c AS-BUILT (2026-09-05): the gesture +
  named-layout management + the startup script.** `halcyon layout list`
  prints every layout of both tiers (name, tier, `shadowed` for a device
  layout a session one hides); on a rich console each name is an
  `obj type=layout` presentation (BEACON.md §12.2, the `layout` type) whose
  menu offers `restore` / `save` / `delete` — the gesture IS the transcript's
  own verb menu, no renderer code. `halcyon layout delete <name>` unlinks the
  session-tier file (the device tier is read-only to the tool; the unlink's
  durability is Stratum's commit — no directory fsync exists). A layout name
  never begins with `-` (so a verb template needs no `--` and no name reads
  as an option) and never ends in the save's `.tmp` (a crashed save's
  residue, hidden from `list`). **The startup script**: once the per-user
  compositor's first tile presents (§14.12) it spawns, AS the user under the
  tile cap mask, `ut --home $home $home/lib/halcyon.rc` if that file exists,
  else `halcyon layout restore default` if the image ships
  `/lib/halcyon/layouts/default` (the first-launch welcome, H-4d), else
  nothing — rio's `-i` idiom; no marker state (an empty rc opts out of the
  welcome). The child is reaped by the compositor (a bounded idle poll while
  it runs) and killed at logout. OWED at H-4d: a restore with TAGGED leaves
  under the session compositor races the compositor's own empty-leaf tile
  spawn for the same leaves (both same-principal; last claim wins) — the
  compositor must leave a tagged empty leaf to whoever tagged it.

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

## 14. The multi-console tile model + the kaua-term substrate (design pass 2026-09-03)

> Same bar as §13 / BEACON.md §12: a session on a lesser model must build this
> without re-deriving a decision. **Operator-ratified 2026-09-03** (the topology
> vote); co-designed with the aux track (Aurora + vivarium), yip call 0044. This
> section is **authoritative for the multi-console model** and **revises §13.1 /
> §13.3 / §13.4** where the single-in-process-brain assumption no longer holds
> (§14.10). Every "exists" claim is verified against the tree (file:line).

### 14.0 The problem + the ground truth

Today Halcyon has **one** `/dev/cons` and **one** transcript: `halcyond` opens
`/dev/consdrain` + `/dev/consfeed` + `/dev/consctl` (`main.rs:278-290`), the same
single-console seam Aurora uses; the kernel `cons` layer is the tty and the
renderer differs only by Beacon posture (rich vs cells). But TAPESTRY §14
ratified "the terminal is the desktop": most tiles START as a console (a `ut` on
its own terminal); a graphical app spawned in a console tile runs inline-live,
promotable to fullscreen; the console shows inline media. The G/H arc built
split/tabbed/zoom/present — **not** N consoles, inline-live, or promotion. This
pass reconciles the ratified anti-window model with the as-built and names the
substrate that gives N terminals.

Ground truth (verified 2026-09-03):

| Fact | Where | Consequence |
|---|---|---|
| `ut` does not open `/dev/cons` — it reads inherited fd 0/1/2, and `ptyhost` already runs it on a **pts slave** (the PTY-4b "pts session dance") | `usr/utopia/shell/src/main.rs:27,337,346` | Native-`ut`-on-a-pts is a solved path, not new impedance. `ut` self-edits its line (libutopia `line_editor`), so it needs only raw pts pass-through. |
| `ptyfs` (`/dev/pts`, `/dev/ptmx`) is the built pts 9P server (PTY-2, I-20) | `usr/ptyfs/src/{main,server}.rs` | The per-tile terminal substrate already exists. |
| Aurora's `Vt` parser moved to the shared crate `usr/lib/vt` (H-2a); `halcyond` + Aurora both consume it | `docs/HALCYON.md §13.4` | The parser is already a shared crate — growing it to full-xterm benefits both. |
| Aurora and Kaua are `no_std` with **`panic = abort`** | `usr/aurora/src/main.rs:870`, `usr/lib/kaua/src/term.rs:21` | A parser panic on hostile bytes aborts the whole process — the crux of the topology decision (§14.2). |
| `halcyond`'s transcript is **cell-addressed** (`TCell`), ingests the byte stream via `t.feed()`, and renders via the `cartoon` executor + fontdue atlas | `transcript.rs:70,76,85`, `main.rs:148,531,616` | A cell-feed from an out-of-process parser is a clean refactor, not a rebuild (§14.3). |

### 14.1 The substrate — (b) uniform-pts-per-tile

Every tile's program (native `ut` **or** a Linux binary) runs on a **pts slave**;
`ptyfs` is the substrate. The substrate fork was three-way: (a) a compositor-served
per-tile `/dev/cons` (the Plan 9 rio model), (b) pts-per-tile (the tmux/kitty model
on the built `ptyfs`), (c) kernel multi-console (a per-tile kernel `cons`). Resolved
to **(b)**: `ut` is already pts-agnostic (above), nothing avoids a VT round-trip
today (`ut` emits VT to fd1; even Kaua's `Terminal` emits cell-diff-as-VT; Aurora is
already a VT-parser → cells), and a Linux binary requires a pts for termios/job
control regardless — so a per-tile `/dev/cons` would be a *second* substrate beside
the pts Linux needs anyway. The heritage (rio per-window terminal) and SOTA
(tmux/kitty ptys, Fuchsia session-per-component) both fuse into "the terminal is the
desktop" on the pts substrate.

### 14.2 The topology — UNIFORM-Y (operator-ratified)

**Every tile is a separate, crash-isolated `kaua-term` process** — native and Linux
uniformly. This **revises §13.1**: `halcyond` no longer hosts the VT parse in its own
process.

The decision is a robustness one, grounded in the tree. A full-xterm parser is a
complex state machine; on **hostile** input (a Linux binary emits arbitrary bytes)
it will eventually hit a panicking edge, and `no_std` `panic = abort` (§14.0) aborts
the *process*. If that process is `halcyond`, the whole environment dies — every
tile. Isolating a hostile-input parser in its own process is exactly the
crash-isolation §13.1 already rests on (why `halcyon-gpu` is a spawned child), not a
departure from it.

The co-design narrowed the fork to the *native* tile — a **hybrid** (native `ut`
in-process, trusted + constrained + low-panic-risk; Linux isolated) versus
**uniform** (every tile isolated). Both aux and main leaned hybrid. **The operator
chose uniform** (2026-09-03): one uniform crash-isolated path and one seam defined
once, over two experiences. So native `ut` tiles are also `kaua-term` processes.
(A pure all-in-process option — accept the crash risk for simplicity — was rejected
by both tracks: a hostile binary crashing the whole environment is not acceptable.)

### 14.3 The seam — (B) feed-cells

The `kaua-term` is a **headless** isolated parser+input process. It holds the pts
master, runs the generic-VT parse (the grown `usr/lib/vt`) into a live `TCell` grid,
and feeds `halcyond`, over the seam, **cells + events**: the live-screen `TCell`
rows, lines as they scroll off, and forwarded control/OSC events (Beacon frames,
bell, exit marks, winsize acks). `halcyond` **keeps** everything it owns today — the
transcript (`TCell` storage), the Beacon-zone/block model, inline media
(`Image`/`Embed`), Helix-modal scrollback selection, and the `cartoon` + fontdue
render — and composites all tiles.

This is the (B) arm of a render-vs-parse fork. The alternative (A) — the `kaua-term`
renders pixels (a weave surface) and `halcyond` is a pure placement orchestrator —
was **rejected**: it drags fontdue + the `cartoon` executor + image decode into the
`kaua-term`, duplicates fontdue per process, and breaks Helix-modal selection (which
needs the *text* in `halcyond`) and inline media (which needs `halcyond` to own the
flow). (A)'s genuine buys (D7 purity; parallel per-process rasterization) do not
outweigh those. **Crash-isolation holds under (B)**: the isolated part is precisely
the hostile-input *parser*; `halcyond`'s renderer only ever touches trusted cells + a
font — far lower panic risk than the parser. (B) is a **clean refactor** of the
as-built: `halcyond` already stores `TCell`s and renders them (§14.0); only the
`t.feed()` parse moves out of process.

**AS-SETTLED — the seam protocol (2026-09-03, yip call 0045; the render
responsibility operator-ratified B).** The wire is one **ordered record stream**
up (kaua-term → halcyond) plus a small stream down. It is **transport-agnostic at
the record level** — the wire codec (`kaua_term::wire`) is independent of how the
bytes move — but the concrete transport is a **pipe pair** per tile (§14.11.6). The
earlier "halcyond-owned Loom ring per tile" is **struck (KT-1.5, 2026-09-03)**: a
Loom `read` requires a dev9p (9P-backed) handle (`kernel/loom.c:1198`), so a pipe is
`-EINVAL`-rejected at submit, and the prior "confirmed `loom.rs:281`" pointed only at
the `Sqe::read` struct builder (which asserts nothing about handle types). halcyond
instead drains the up-pipes and unifies its wait by making the Loom ring itself
pollable (§14.11.7 / §14.11.7a). The record set:

- **Up** (kaua-term → halcyond): `CellDiff{ changed (row,col,cell)[], cursor(row,col,vis) }`
  (the live screen) · `ScrollOff{ rows: cell[] }` (normal-mode lines off the top →
  the transcript) · `Control{ osc1936_raw | bell | title | exit(code) | winsize_ack }`
  (the kaua-term forwards OSC 1936 Beacon frames **raw** — halcyond keeps the Beacon
  parser, R5) · `Mode{ normal | alt_screen }`. **Ordering is load-bearing** (a Beacon
  zone-frame must land at the exact point between the cells it separates), so the
  producer **flushes a pending `CellDiff` before every `Control`/`Mode`/`ScrollOff`
  and at end-of-input-chunk**, never coalescing a frame across a zone boundary.
  halcyond replays the stream in order; the block model is exact (a scroll-off after
  a zone-open lands in the new zone). Within a `CellDiff` the cells are position-keyed,
  so only the *boundary* order is load-bearing.
- **Down** (halcyond → kaua-term): `Key{ KeyEvent }` (halcyond routes
  post-chrome-chord input to the focused tile; the kaua-term xterm-encodes honoring
  DECCKM/keypad → the pts) · `Resize{ cols, rows }` (the kaua-term sets pts winsize +
  the app gets `SIGWINCH`).
- **The wire cell is the shared `usr/lib/vt::Cell`** (self-contained `ch` + style), so
  the wire is a slice of one struct with no translation; halcyond's per-block `TCell`
  style-interning is a consumer-internal detail on ingest. This requires the H-2a crate
  to reach the aux tree (a build-prep sync at KT-2; §14.10).
- **Tier**: under B halcyond renders, so the render tier is RICH for every Halcyon
  tile; the pts advertises `BEACON=rich` (set at kaua-term spawn — §14.6's advertise
  side), so a tier-aware program in the tile emits rich markup.

The native-`ut` VT-round-trip (a native Kaua app feeding cells more directly than
emitting VT to be re-parsed) stays a **v1.x optimization**; v1.0 native `ut` emits VT
to its pts and the kaua-term parses it, as terminals do. The aux track's producer side
of this contract is `docs/KAUA-TERM.md`.

### 14.4 The split

- **`halcyond`** — compositor + **transcript-orchestrator** + placement/promotion.
  Owns: the transcript, Beacon parse, inline media, Helix selection, fontdue + the
  `cartoon` render, the layout/chrome/menus, and per-tile composition. Spawns a
  `kaua-term` per tile; consumes its cell-feed. **Not a VT host** (the §13.1
  revision).
- **`kaua-term`** (aux track; `docs/KAUA-TERM.md`) — the isolated per-tile
  parser+input process: the grown VT parser, the pts-master host, the
  `KeyEvent → xterm` re-encode, and the seam's `kaua-term` side. Both modes (native
  + Linux) uniformly isolated.
- **kernel / vivarium** — **C2-k1c** (the four termios/winsize ioctls
  `{TCGETS, TCSETS/W/F, TIOCGWINSZ, TIOCSWINSZ}` reached through the Linux phenotype
  to the *existing* `ptyfs` line discipline — a Linux-tile enabler; native `ut`
  needs none of it) and **C2-k3** (the job-control ioctls
  `{TIOCSPGRP, TIOCGPGRP, TIOCSCTTY}` + `SIGTTIN`/`SIGTTOU` — Linux `bash` fg/bg/^Z;
  a follow-on, not MVP-blocking). Cooked mode itself is **already built** in `ptyfs`
  userspace; C2-k1c is only the phenotype's reach to it.
- **`aurora`** — **unchanged.** It remains the CELLS-tier renderer and the trusted
  `/dev/cons` rasterizer (the SAK sink) for aurora-mode sessions; the multi-console
  neither subsumes nor alters it. What the `kaua-term` shares with `aurora` is the
  `usr/lib/vt` **parser** only, not `aurora`'s renderer — the `kaua-term` does not
  rasterize (halcyond does, under B). So the co-design's earlier "R2 subsume" is
  refined under (B) to a **parser-share**: one `usr/lib/vt` crate that `aurora`, the
  `kaua-term`, and halcyond's ingest-side `Cell` all consume, three renderers apart.

### 14.5 Trusted path / I-27 — unchanged and orthogonal

Grounded in TAPESTRY §18.7: on virtio-gpu-only media (QEMU) the trusted path stays
**serial** (BREAK-SAK); the single `/dev/cons` is a renderer-held drain/feed fid pair
(`halcyond`'s), and a framebuffer SAK episode (simplefb-class boards) suspends the
renderer while the kernel paints. The multi-console model adds per-tile ptys **below**
that single trusted `/dev/cons`, all **uniformly untrusted** (exactly as rio windows
are never the trusted console). The trusted conversation never touches a tile pts, so
the trusted path is untouched and **orthogonal to the X/Y topology** — it does not
discriminate between in-process and per-process tiles.

### 14.6 winsize + beacon relocate per-tile, together

Under one `/dev/cons` the renderer reports winsize and its render **tier** through
`consctl`: the `winsize <cols> <rows>` verb (#55) and the `beacon <tier>` verb —
paired as the two renderer-authority verbs a `CCONSWINSZONLY` consctl may write
(`cons.c:2138`); `ut` reads `/dev/beacon` and exports `BEACON=rich`, which programs
read to choose output. Under multi-console **both** relocate to the per-tile pts,
**together**:

- **winsize** → the pts (`TIOCGWINSZ`); the compositor is the geometry authority,
  sets the tile's pts winsize, and a resize raises `SIGWINCH`. The per-pts `ptyfs`
  winsize model already exists.
- **beacon tier** has two sides that **must match per tile**: the **render** side
  (a compositor-set rasterizer flag the `kaua-term` honors — the tier is the
  compositor's choice, not the app's) and the **advertise** side (the program reads
  its tile's tier via `BEACON` on the tile's pts). A CELLS tile whose program reads a
  stale `BEACON=rich` would emit TTF-assuming output the tile cannot honor — so the
  advertisement is per-tile, not global. Retiring the `CCONSWINSZONLY` console
  special-case for tiles moves winsize **and** beacon onto the per-tile pts ctl.

### 14.7 Inline media — native, out-of-band

`cat picture.png` → an inline image is a **native** path: a `display`/type-aware
coreutil or the shell emits an `Embed`/`Image` to `halcyond` **directly** (out of
band), **not** PNG bytes down the pts — raw image bytes down the pts hit the VT
parser as garbage. This matches NOVEL §3.4 (bytes-in-text rejected as the media
mechanism) and keeps image decode in `halcyond` (Rust, the format-fuzz surface).
Terminal image-escape protocols (sixel / kitty / iTerm) are **v1.x**.

### 14.8 Inline-live graphical apps + promotion (TAPESTRY §14 concretized)

A graphical app spawned in a console tile (the operator's `tyr-quake` example) is a
separate `libtapestry` client surface, placed **inline-live** (`Embed`) in the tile's
transcript flow (TAPESTRY §14's D5 placement-transparency); it promotes inline-live →
pane-zoomed → tab/display (D6 live-reparent), the client rendering the same surface
throughout. **Under UNIFORM-Y this unifies cleanly**: the `kaua-term` is *also* a
client surface `halcyond` composites, so a terminal tile and an inline graphical app
are the **same shape** — a composited client surface in a tile's flow. "Start a game
inline, promote it to fullscreen" and "a terminal in a tile" are one mechanism.

### 14.9 Build order

1. **`kaua-term` native mode + the seam + `halcyond` per-tile composite** →
   **unblocks H-4d** (the welcome's two `ut` panes). Native `ut` already has full
   pts job control today (`t_tty_*` + the PTY-4b session dance), so the *terminal*
   half needs no kernel work; the *ingest* half adds **one small kernel enabler** --
   `KObj_Loom` pollable + SQPOLL for the compositor ring (§14.11.7a, KT-1.5,
   operator-ratified Option 1 2026-09-03), which also fixes the pre-existing
   frame-coupled console latency. This is KT-1 (`docs/KAUA-TERM.md`).
2. **C2-k1c** → Linux tiles (termios/winsize reach).
3. **C2-k3** → Linux job control (fg/bg/^Z).

The gate (H-4d) never waits on the kernel work.

### 14.10 §13 supersessions + audit + open items

- **§13.1** "`halcyond` … owns … the VT core instances … in-process" → **superseded**
  for per-tile terminals: the `kaua-term` process owns the parse (§14.2/§14.3).
  `halcyond` remains the single brain for the transcript, render, layout, and
  composition.
- **§13.3** "ingests the byte stream via `t.feed()`" (in-process) → the generic-VT
  parse **moves to the `kaua-term`**; the transcript **consumes cells** over the seam
  (§14.3). The `TCell` model and everything above the parse are unchanged.
- **§13.4(a)** "the raw-VT pane class hosts a full `Vt` grid … in `halcyond`" →
  **superseded** by the `kaua-term` process; the shared `usr/lib/vt` crate is grown
  to full-xterm (DECSTBM/margins, SU/SD, origin mode, wide chars, SGR residue) and
  consumed by the `kaua-term`. This is a **shared-crate** change: `usr/lib/vt` (H-2a)
  is absent in the aux worktree (its `vt.rs` is still in Aurora there), so the growth
  is a coordination point — H-2a must reach the aux tree, or the crate is coordinated
  at the seam.
- **Audit**: the `kaua-term` is a hostile-input parse surface — it joins the
  format-fuzz audit class (like the Beacon parser). C2-k1c/C2-k3 are syscall/ioctl
  surfaces (an AUDIT-TRIGGERS row at their build). The seam is a new IPC parse surface
  on `halcyond`'s side (bounds-check the cell-feed like the 9P wire).
- **Open**: the seam **protocol** is now **SETTLED** (§14.3 AS-SETTLED, yip call 0045);
  the **halcyond ingest model** (the grid live-area + scrollback transcript, the
  render composition, spawn/multiplex/focus/teardown) is designed in **§14.11**
  (KT-1.5, 2026-09-03) -- the finding that §14.3's "clean refactor" under-estimated
  it (halcyond is single-console + flow-based, no grid, no spawn). What remains to
  build is KT-1.5 (§14.11.11 stages); what remains cross-tree is the **H-2a crate
  sync** to the aux tree (build-prep, aux's cherry-pick at KT-2), the native-`ut`
  VT-round-trip **v1.x optimization**, and sixel/kitty + JPEG → v1.x.

### 14.11 The halcyond ingest model -- the live grid + the scrollback transcript (KT-1.5 design)

**Why this section (the finding that drove it, 2026-09-03 run 21).** Building the
kaua-term producer side surfaced that §14.3's "clean refactor -- only the
`t.feed()` parse moves out of process" materially under-estimates the halcyond
side. As-built (Explore-mapped): halcyond is a **single-console, flow-based**
renderer -- ONE `Transcript` (`transcript.rs:221`, a flowed deque of blocks; no
fixed grid, no alt-screen buffer), ONE byte source (`/dev/consdrain`,
`main.rs:278` -- a kernel console mirror; it owns no pty and **spawns no child
process**), and its "tiles" (`chromeset.rs`) are tag-bar chrome for the
compositor's leaves, not terminal content. The seam sends a **grid** model
(CellDiff on rows x cols). So KT-1.5 is not "move the parse out"; it is: give
halcyond a per-tile grid it never had, child-spawn machinery it never had, a
multiplex of N record streams, and a transcript refactored to ingest
grid+scrollback. This section pins that model (scripture before code, the
design-conversation pattern; operator chose "scripture design pass first").

**14.11.1 The per-tile model = a live grid + a scrollback transcript.** Each tile
holds two structures, not one:

- **The live grid** -- a fixed `rows x cols` cell buffer (`vt::Cell`), the current
  terminal screen. It is what CellDiff mutates. It is the §13.4(a) "raw-VT pane
  grid" resurrected as the *universal* live-screen (both modes), now fed by the
  kaua-term's records rather than an in-halcyond `Vt`.
- **The scrollback transcript** -- the existing block model (`transcript.rs`,
  §13.3), now fed by **ScrollOff** (lines that left the top of the grid), cut into
  blocks by **Beacon zones**. It is pure history: everything that has scrolled off.

They are **separate** because the grid spans zone boundaries: the last `rows`
lines of a session routinely straddle a prompt (the tail of one command's output
+ the next prompt), so the grid cannot be "one zone's block". The grid is
zone-agnostic; the transcript carries the zone structure. This supersedes §13.3's
"the open block is the live tail" -- under §14 the live tail is the **grid**, and
the transcript's blocks are finalized history.

**14.11.2 The record -> model mapping.**

| Up record | Applied to |
|---|---|
| `CellDiff{changed, cursor}` | the live grid (position-keyed cell writes + cursor) |
| `ScrollOff{rows}` | append rows to the current history block (a new block if a zone just opened) |
| `Control(Osc1936Raw)` | fed to `beacon::wire::parse` -> the zone/block cut + span state (exactly as `Transcript::feed` does today, `transcript.rs:407`) |
| `Control(Bell)` | the bell affordance (visual/log; no kernel bell) |
| `Control(Title)` | the tile's OSC-0/2 title |
| `Control(Exit(code))` | the tile's exit latch (the child is gone; teardown) |
| `Control(WinsizeAck)` | resize handshake bookkeeping |
| `Mode(Normal\|AltScreen)` | the render mode (14.11.3) |

The ORDER is load-bearing and already guaranteed by the producer (a pending
CellDiff is flushed before every ScrollOff/Control/Mode) -- so a zone frame lands
at the exact point between the cells it separates, and a scroll-off after a
zone-open lands in the new zone.

**14.11.3 The render composition.**

- **Normal mode**: the scrollback blocks (flow layout, `layout.rs`, cursor-anchored
  like #55) render above; the **live grid** renders as the tail (a fixed-height
  grid region at the bottom). The viewport shows the tail by default; scrolling up
  reveals history. The seam is contiguous: a line leaving the grid (ScrollOff)
  becomes the top-of-history-block's newest line.
- **Alt-screen mode** (`Mode(AltScreen)`, e.g. vim): render the **live grid only**,
  full-tile; the scrollback is frozen and hidden. On `Mode(Normal)` the scrollback
  returns and the grid resumes as the tail. This finally renders full-screen apps
  correctly -- today halcyond sets `raw_vt_intent` and *paints nothing*
  (`transcript.rs:932`).

**14.11.4 Beacon zones + the grid.** Beacon frames arrive as `Control(Osc1936Raw)`
interleaved in stream order; halcyond feeds them to the *same* `beacon::wire`
parser it uses today, driving the *same* block-cut / span state on the scrollback
transcript. The grid is not zoned; the "current zone" is simply whichever block
ScrollOff is currently appending to. A zone-open freezes the current block and
starts a new one; subsequent ScrollOff lines land there.

**14.11.5 Selection + inline media.** Helix-modal selection addresses
`(block, item, col)` over the scrollback AND the live grid (the grid is selectable
as the live region -- a virtual trailing block; yank re-derives cell text as
today). Inline media (`Image`/`Embed`) stays the **out-of-band native seam**
(§14.7): the grid is text-only; a graphical app in a tile promotes to a Tapestry
surface (§14.8), it does not paint pixels through the cell stream.

**14.11.6 Spawn.** halcyond spawns one `kaua-term` per **leaf tile**. The
enumeration hook already exists: `ChromeSet::reconcile` (`chromeset.rs:129`)
iterates `parse_leaves` (`chrome.rs:51`) over the compositor's `layout` file per
`u32` leaf id, computing the create/keep/drop diff each relayout -- KT-1.5 hangs
per-leaf spawn/teardown off that same diff. The child is launched
`kaua-term <cols> <rows> [prog]` with **fd 0 = the down pipe** (halcyond->child)
and **fd 1 = the up pipe** (child->halcyond); fd 2 = stderr to halcyond's log.
The pts is internal to the child; spawn installs only the child's three slots, so
the app the child hosts never sees halcyond's pipes (the non-inheritance ptyhost
relies on). This is halcyond's FIRST child-spawn -- new machinery
(`Command`/`Stdio::File` on the two pipe ends).

**14.11.7 Multiplex -- the unified `poll(2)` (CORRECTED; operator-ratified Option 1,
2026-09-03).** The tile up-channels are **pipes**, not Loom-registered handles: a
Loom `read` requires a dev9p handle (`kernel/loom.c:1198`; a pipe `-EINVAL`s at
submit), and the earlier "confirmed `loom.rs:281`" was a misread of the `Sqe::read`
struct builder, which says nothing about handle types (triple-confirmed by the KT-1.5
research pass -- Weft is out too: its readiness ring is single-source and its
blocking park is unwired, so the only wake-on-any-of-N is a Loom ring, and that needs
dev9p handles). halcyond therefore multiplexes every readiness source in **one
`poll(2)`**:

```
poll { tapestry-EventRing loom-fd | N tile up-pipes | /dev/consdrain }
```

waking promptly on any -- a byte on a tile pipe, a surface event, or console output.
On a tile pipe -> drain records -> that tile's `FrameDecoder` -> its grid; on the
loom-fd -> reap the CQ in userspace + route surface events; on consdrain -> the
existing console drain. Pipes and `/dev/consdrain` are already pollable
(`kernel/pipe.c`; `cons_drain_poll`); the **new** piece is making the Loom ring
pollable (§14.11.7a). This also eliminates a pre-existing **FRAME-coupled** console
latency: halcyond/aurora block on the ring and drain consdrain only per frame ->
~16 ms active, up to ~67 ms idle, and a full stall on an occluded surface
(`tapestryd/src/main.rs:124`, `aurora/src/main.rs:21`). With a pollable ring the wait
is byte-driven, not frame-driven -- the fix lands system-wide (aurora shares
`EventRing`).

**14.11.7a The kernel enabler -- `KObj_Loom` pollable + the compositor ring runs
SQPOLL (operator-ratified live 2026-09-03).** Two pieces, both small and
well-precedented:

- **`KObj_Loom.poll`.** `poll_scan_one` (`kernel/poll.c:213`) gains a `KOBJ_LOOM`
  arm calling a new `loom_poll(l, events, pw)` that registers the poller on the ring's
  **existing** `l->cq_waiters` list and reports `POLLIN` iff `loom_cq_ready(l) > 0`.
  The wake is already wired -- every CQE post fires
  `poll_waiter_list_wake(&l->cq_waiters)` (`kernel/loom.c:374,678`) -- so this is a
  direct **register-then-observe** on the same list `loom_wait_for_completions`
  already uses (`loom_cqw_cond`, `kernel/loom.c:1758`). **No new spec:** unlike the
  cons `.poll` (LS-8a / `cons_poll.tla`, whose IRQ-context RX forced a deferred
  mgr-kthread relay), the Loom CQE wake ALREADY runs in process/kthread context, so it
  is a plain instance of the poll_waiter_list I-9 pattern (`poll.tla` lineage) --
  validated by prose + the existing poll/loom buggy cfgs + runtime tests + the audit,
  per the standing spec-first suspension (no re-enablement warranted).

- **SQPOLL for the compositor ring.** A poll on a Loom ring is only meaningful if CQEs
  post **without** the owner calling `enter` (else nothing pumps the 9P session while
  halcyond sleeps in `poll`). `LOOM_SETUP_SQPOLL` already provides this -- a kernel
  poll-thread admits + pumps + posts autonomously (kernel-tested -- `test_loom.c`'s
  `sqpoll_*` tests; it had **no** EL0 consumer until KT-1.5-kernel -- `loom-bench`
  runs `flags=0` -- so the loom-smoke poll leg is the FIRST EL0 SQPOLL driver).
  `libtapestry`'s `EventRing` gains an SQPOLL setup mode + a userspace
  CQ-reap path (reap without `enter`) + the ring fd exposed for `poll`; the tapestry
  session is one-per-ring (the `EventRing` invariant), so the SQPOLL kthread's
  single-session pump is sound (no cross-session starvation).

**Audit-bearing.** Both `kernel/loom.c` and `kernel/poll.c` are audit-trigger surfaces
(I-29/I-30 Loom completion integrity; I-9 poll wake). The `KObj_Loom.poll` arm + the
SQPOLL compositor adoption get an `AUDIT-TRIGGERS.md` row + the vault
`sub-kernel-loom` / `sub-kernel-poll` / `sub-libtapestry` updates at implementation,
and join the batched KT-1 audit.

**14.11.8 Resize.** halcyond is the geometry authority (it owns the tile rects).
On a tile resize it sends `Resize{cols, rows}` down; the kaua-term sets the pts
winsize (kernel `SIGWINCH` to the fg pgrp) and resizes its `Vt`, then emits a full
CellDiff of the new grid, which halcyond applies to the (resized) live grid. The
scrollback reflows by re-running the pure `layout()` at the new width (§13.3,
correct-by-construction).

**14.11.9 Focus + input.** halcyond routes post-chrome-chord keyboard input to the
**focused** tile only: it filters its own chrome chords first (menu, split, zoom,
focus-move -- the existing `input.rs` path), then encodes the remaining KeyEvent as
`Key{KeyEvent}` down the focused tile's pipe; the kaua-term xterm-encodes it
(honoring DECCKM) to the pts. Focus is the compositor's `l.focused` / the `*` in
the layout (`chrome.rs`), already parsed.

**14.11.10 Teardown + the trust boundary.** The kaua-term is the crash-isolated
**hostile-input** parser, so halcyond's record ingest is a trust boundary: it
bounds-checks the wire (the `kaua_term::wire` `FrameDecoder` -- MAX_FRAME, checked
fields, no untrusted pre-alloc; the "bounds-check like the 9P wire"). A
`WireError` (oversize/malformed), a `Control(Exit)`, or an up-pipe EOF tears down
**only that tile** -- its pipe fds, its poll-set slot, its grid+transcript, and
its Tapestry content surface -- never the whole environment. A tile whose child
dies shows an exit affordance; a relayout that drops the leaf reaps it.

**14.11.11 Build stages (KT-1.5).**

- **KT-1.5a -- the transport prover (`kaua-term-probe`).** A boot probe (NOT
  halcyond) spawns ONE kaua-term hosting a known program over a real pipe pair,
  drains its records with **`t_read`** (a blocking pipe read -- Loom does not read
  pipes), decodes them, and asserts the hosted output + a clean `Control::Exit`.
  Boot-proves the untestable process-level surface: the pts host, the two blocking
  threads, the codec over a real pipe. (Run 21's probe becomes this once its
  Loom-over-pipe read is replaced with `t_read`.)
- **KT-1.5-kernel -- `KObj_Loom` pollable + SQPOLL (§14.11.7a).** The enabler for the
  unified wait: the `poll_scan_one` `KOBJ_LOOM` arm + `loom_poll`; `libtapestry`'s
  `EventRing` SQPOLL setup + userspace CQ reap + the ring fd exposed for `poll`. Its
  own kernel-test (`test_loom.c` extension) + the SMP gate; audit-bearing (Loom + poll).
- **KT-1.5b-i -- the unified `poll` (consdrain only). LANDED.** `libtapestry`
  gains `connect_sqpoll` / `adopt_flags` / `poll_fd` (additive; `connect` stays
  non-SQPOLL, so aurora is untouched); halcyond opens its event set SQPOLL and
  replaces the block-on-ring (`EventRing::wait`) with ONE `poll { ring.poll_fd()
  | /dev/consdrain }`. Fixes the frame-coupled console latency (shell output
  wakes the renderer at once, not at the next compositor frame tick) and removes
  the F7 root cause (the SQPOLL kthread demuxes the session's parked read replies
  continuously -- see `docs/reference/150-halcyond.md`). No tiles yet. Proven by
  the default suite (aurora-no-regression, `loom.poll` unit, the `loom-smoke`
  SQPOLL leg) + the `ls-halcyon.exp` graphical E2E (render + rich + reflow +
  menus, all on the SQPOLL ring).
- **KT-1.5b-ii -- halcyond ingest -> the model. RESHAPED by 14.12.** The old
  "halcyond spawns ONE kaua-term + renders" framing is superseded by the per-user
  compositor decision (14.12, operator-ratified 2026-09-04): the spawn+render lands
  INSIDE a per-user halcyond login spawns as the user, staged as **KT-1.5d-1** (the
  per-user bootstrap + the aurora->halcyond display handoff), **KT-1.5d-2** (one
  session tile: CellDiff->grid / ScrollOff->scrollback / Mode->render-mode + the
  normal/alt render, on the ii-a `Tile` model), **KT-1.5d-3** (multi-tile -> H-4d).
  The KT-1.5b-ii-a `Tile` MODEL (grid + ingest, host-tested, @a0324198) feeds
  KT-1.5d-2 directly.
- **KT-1.5c -- multi-tile.** Per-leaf spawn/teardown off `reconcile`, the N-pipe
  multiplex in the unified poll, focus-routed input (Key/Resize down), per-tile
  composite -> **unblocks H-4d**.

Then the batched KT-1 audit (KT-1.1..1.5; format-fuzz + PTY-master + the new
ingest trust boundary) + the boot gate, then push.

**14.11.12 Invariants + audit.** The ingest joins the **format-fuzz** audit class
(halcyond parsing an untrusted per-tile stream). A tile's kaua-term crash, a
malformed/oversize frame, or its exit MUST be contained to that tile (no
cross-tile effect, no halcyond death) -- the I-27-adjacent isolation the whole
uniform-Y topology (§14.2) rests on. The grid is text-only (no pixel path through
cells; graphical apps promote to a Tapestry surface, preserving D7). An
AUDIT-TRIGGERS row lands with the KT-1.5 code.

### 14.12 The per-user session compositor -- login spawns halcyond as the user (operator-ratified 2026-09-04)

**The problem this resolves.** 14.11.6 has `halcyond` spawn the session's `kaua-term`
tiles, but the session `ut` needs the **user identity + the encrypted `/home/<user>`
that only `/sbin/login` establishes** (`CAP_SET_IDENTITY` + the per-user DEK unlock,
A-5). The as-built `halcyond` is a **system** renderer spawned by joey *before* login
(`joey.c:11255-11319`), so "halcyond spawns the session tile" collides with "login
owns the user identity." Closing that gap by giving the system halcyond
identity-delegation power is an **I-22 ambient-authority hole** (a system daemon that
can run code as any user). Ground truth (Explore, run 23): the trusted path is
**orthogonal** (SAK is a kernel `/dev/cons` BREAK -> `proc_console_sak` `proc.c:2091`;
a pts carries none of it; `/dev/consfeed` hardwires `is_break=false`; the graphical
trusted path is unbuilt, the live one serial-only), so the crux is **identity, not the
trusted path**.

**The decision (operator-ratified 2026-09-04, over system-halcyond-delegation and
prove-first-defer).** `halcyond` is a **per-user session compositor**, spawned by
`/sbin/login` **as the user**, not a system renderer. This is the Plan 9 rio idiom
(a per-user window system started after login), the Wayland compositor-per-session
model, and the Fuchsia session-per-user model, fused onto Thylacine's per-principal
`tapestryd`. **Zero identity delegation** -- the only identity-stamp is login's, which
login already holds.

**The model.**

1. **Pre-login (system): aurora renders the console.** joey spawns `aurora` (the
   existing fbcon system renderer, `SPAWN_PERM_CONSOLE_RENDERER`); the getty runs
   `/sbin/login` on the kernel `/dev/cons`; aurora mirror-renders the login prompt
   via `/dev/consdrain`. This is aurora's existing role -- **the `THYLACINE_HALCYON`
   lever that made joey spawn halcyond as the SYSTEM console renderer is retired**;
   halcyond is now per-user/post-login and never the console renderer.
2. **Login (on `/dev/cons`, aurora-rendered).** `/sbin/login` authenticates via corvus
   and establishes the user identity + unlocks + mounts `/home/<user>` (A-5,
   unchanged). Then, instead of spawning `ut` on `/dev/cons`, login spawns a **per-user
   `halcyond` AS the user** -- `Command::new("/bin/halcyond").identity(pid,gid,&supp)`,
   exactly as it spawns `ut` today (`login/main.rs:1236`) -- and `wait()`s it; the
   halcyond exit is logout.
3. **Session (per-user halcyond).** The per-user halcyond:
   - connects to the **system** `tapestryd` (`/srv/tapestry`) as the user. Connecting
     is ungated and classifies the conn `Actor::Session(user_principal)` -- the
     per-principal design already anticipates ordinary-user renderer clients
     (`server.rs:15185-15202`); no special cap.
   - presents a fullscreen surface and hosts the session's terminals as **kaua-term
     processes** (14.2 uniform-Y -- separate + crash-isolated), spawned **as itself**
     (the user's identity; plain `t_spawn`, no cap, no delegation). It reuses the
     current halcyond render brain (transcript / chrome / menus / fontdue / cartoon,
     the ii-a `Tile` model) but ingests each tile's record stream instead of the
     `/dev/consdrain` mirror.
   - routes input (tapestryd `TEV_KEY`/`TEV_PTR` on its EventRing, the same input path
     aurora uses) to the **focused tile's pts down-channel**, not `/dev/consfeed`.
   - the session `ut`s run in the tiles (pts job control -- the Ctrl-C axis is the
     pts fg pgrp, not the `/dev/cons` owner), never on `/dev/cons`.
4. **The display handoff -- the compositor backgrounds the console renderer
   (operator-ratified 2026-09-04; the Plan 9 rio / Fuchsia-Wayland idiom; the
   trigger AMENDED 2026-09-05 by the KT-1 audit, C-F6 -- the record is under
   KT-1.5d-1b below).** tapestryd's scanout follows the layout AND a **declared
   seat** (`Comp::reconcile`): the session compositor DECLARES the handoff by
   writing `session on` on its own ctl conn before its first surface hosts
   (Session-principal-gated; one declared conn per display). The seat is held
   by a conn WHILE IT HOSTS: any Session conn takes it over while the holder
   hosts nothing (an idle declaration holds no display; a crashed compositor's
   conn is un-declared the moment its EOF is serviced), and a holder hosting
   leaves keeps it against every newcomer, the user's own programs included --
   the newcomer then runs UNDECLARED, its tiles beside the console like any
   user window, never a login loop. The compositor re-issues `session on` after
   its first surface hosts and takes that verdict, closing the idle window
   between its declaration and its first mint. While the DECLARED conn hosts a leaf, the SYSTEM
   leaves (a sentinel principal: the console renderer aurora, and any system
   client) are **BACKGROUNDED** -- excluded from the scanout decision, not
   composited, and not FRAME-ticked. So aurora + a per-user halcyond both
   presenting fullscreen do NOT tile: the root collapses to the session
   (`Direct(halcyond)`), and aurora's own FRAME-driven loop goes dormant (no FRAME
   -> it blocks in `wait_event`; it keeps its surface + ring the whole time,
   staying observable). On logout (halcyond exits; its conn retires, which clears
   the declaration), reconcile re-runs, aurora is no longer backgrounded, the
   FRAME clock ticks it again, and it resumes + repaints -> `Direct(aurora)`,
   rendering the next login. A user program that merely draws a window never
   takes the display (it tiles beside the console as before).
   **NO new primitive, NO drain-poll.** The "emergent, drain-idle resume" of the
   earlier draft was WRONG: `SYS_PUTS`/`say!` routes through `cons_emit` (kernel #76),
   so the console drain carries ALL daemon diagnostic output and is NEVER idle during
   a session -- a drain-based resume flaps (relinquish -> tapestryd logs -> drain ->
   resume -> ...). The declaration is **backward-compatible**: with no declared conn
   present (every pre-session + gfx-test path) nothing backgrounds and the pre-existing
   `Direct(n)` iff one display-sized leaf else `Composed` logic is byte-identical.
   **aurora is UNCHANGED** -- the whole mechanism lives in tapestryd's reconcile +
   frame_tick + compose.
5. **Trusted path (I-27) -- orthogonal, unchanged (14.5).** `/dev/cons` + the SAK
   stay the kernel trusted path (serial today, kernel-sink future); whichever renderer
   is active suspends during a trusted episode (when the graphical suspension lands).
   login's credential entry stays on `/dev/cons`, the path the future graphical trusted
   sink will protect -- it is **never** buried in an untrusted kaua-term pts (which
   would let a userspace terminal see the passphrase). The tiles are below the trusted
   console, never trusted.

**What is reused vs retired.** REUSED: the entire halcyond render brain (H-2 transcript,
H-3 chrome/menus/status, the Daylight stylesheet, fontdue + cartoon, the KT-1.5b-ii-a
`Tile` grid+scrollback model) + libtapestry + the KT-1.5b-i unified poll. RETIRED for
the session: halcyond as the **system console renderer** -- it no longer opens
`/dev/consdrain`/`consfeed`/`consctl` (the session halcyond is a **variant** that skips
that trio, `main.rs:278-296`, and reads pts tiles instead) and no longer holds
`g_console_renderer` (aurora does, for the pre-login console). The `THYLACINE_HALCYON`
pool lever + the ls-halcyon system-renderer test are superseded by a per-user-session
graphical test.

**Capabilities (I-22 clean; Explore-verified).** The per-user halcyond needs: (a)
connect to tapestryd -- **none** (ungated); (b) spawn kaua-term processes -- **none**
(same-identity children); (c) route input -- **none** (pts writes). It does **not** need
`CAP_SET_IDENTITY` (it is a user, not an identity-stamper) or `SPAWN_PERM_CONSOLE_RENDERER`
(it does not read the kernel console mirror). The single identity-stamp is login's,
already held.

**Sequencing (a multi-chunk arc; supersedes the old KT-1.5b-ii-b single-tile framing).**
14.12 is scripture; implementation stages:
- **KT-1.5d-1 -- the per-user halcyond bootstrap.** Split a/b to de-risk (the
  handoff touches the load-bearing fbcon; the bootstrap is additive + gated):
  - **KT-1.5d-1a (additive, gated -- default boot untouched).** login reads the
    `/lib/halcyon/session` lever and, when `on`, spawns `/bin/halcyond --session`
    AS the user; a session-variant halcyond (`session_main`) that skips the
    `/dev/cons` trio, connects to tapestryd as `Session`, and presents a BLANK
    fullscreen surface (content to compose ALONGSIDE aurora -- Composed). Prove:
    login -> per-user halcyond presents (`halcyond: session up`, a post-present
    marker). Graphical E2E `ls-gfx-session` (serial-driven: aurora is serial-loud
    without `thylacine.display=gpu`; halcyond's markers ride the diagnostic UART
    regardless).
  - **KT-1.5d-1b -- the clean display handoff.** tapestryd's reconcile backgrounds
    the console renderer (aurora) when a session leaf is present -> `Direct(halcyond)`;
    on logout aurora un-backgrounds and its FRAME-driven loop resumes (14.12 step 4).
    Touches tapestryd's scanout machine (audit-bearing); **aurora is unchanged** (no
    FRAME -> dormant; FRAME -> renders). No new primitive; backward-compatible (the
    priority is inert with no session leaf present).
    **AMENDED 2026-09-05 (the KT-1 audit, C-F6; self-resolved under the standing
    autonomy, recorded for the operator):** "a session leaf is present" is NOT
    "a user-principal leaf is present". `principal_is_session` is true for every
    real user, so the d-1b trigger put the console renderer to sleep whenever any
    user program (DOSBox, tapestry-demo, the battery) drew a window from the
    console shell -- a default-boot behaviour change shipped under a "byte-
    identical console path" claim. The display handoff is an EXPLICIT act of the
    session compositor: it writes `session on` on its own ctl conn before its
    first surface (Session-principal-gated; one declared conn per display at a
    time; cleared when the conn retires -> the console un-backgrounds). Only a
    DECLARED session backgrounds the console; a user program that merely draws
    never does (it tiles beside the console as before, and a zoom hides it the
    old way). **The seat is the principal's, not the conn's (the round-2
    finding C2-F1):** a first-come slot with no takeover let ANY Session conn
    that wrote `session on` -- a same-user program, an orphan of a previous
    user -- hold the seat until it died, and the compositor's fatal reaction
    to the refusal turned every later login into the C-F12 re-prompt loop.
    Now any Session conn takes the seat over while the holder hosts nothing,
    and a holder that hosts leaves answers E_BUSY to every newcomer (round 3
    F6 removed the same-principal exception: a user's own program stealing
    the seat from the user's LIVE compositor degraded it for the session) --
    which halcyond retries briefly (a seat mid-handover) and then TOLERATES:
    it runs undeclared, beside the console, and says so; it re-declares after
    its first surface hosts, so an idle usurper in that window cannot leave
    it mislabelled. The declaration clears AFTER the dying
    conn's surfaces retire (one transition to `Direct(console)`, not N-1
    composed passes of dead tiles beside it). The declared conn also hears
    every structural layout change on one of its surfaces (`TEV_LAYOUT`,
    kind 10): a split of an EMPTY leaf fans no CONFIGURE and no FOCUS, and
    the empties it must claim have no other channel. The declared conn also mints surfaces up to the renderer's cap (one
    tile per pane), and a surface's backgrounded state derives from its leaf's
    tree flag alone. The console renderer's DISPLAY-level chrome (a status bar,
    a placed menu) is NOT backgrounded with it: aurora has none, and a renderer
    that does (the retired halcyond-console lever) must dismiss/hide it on the
    handoff (C-F11, open).
- **KT-1.5d-2 -- one session tile.** the per-user halcyond spawns ONE kaua-term (as
  the user) hosting `ut`, folds its up-pipe into the unified poll, ingests via the
  ii-a `Tile` model, and renders it (normal = scrollback + grid tail; alt = grid only,
  14.11.3). This is the old ii-b render, now inside the per-user compositor.
- **KT-1.5d-3 -- multi-tile.** per-leaf spawn/teardown + N-pipe multiplex +
  focus-routed input (the old 1.5c) -> **unblocks H-4d** (the welcome's two `ut` panes).

**Invariants + audit.** No new I-22 surface (zero delegation -- the property to
prosecute is exactly that: the per-user halcyond holds no identity-stamp and no
console-renderer role). I-27 unchanged (14.5). The session halcyond joins the
format-fuzz audit class (ingesting untrusted per-tile record streams, 14.11.12). The
login->per-user-halcyond spawn + the aurora handoff are the new privilege-adjacent
surfaces (an AUDIT-TRIGGERS row at KT-1.5d-1). The kaua-term parser stays the
crash-isolated hostile-input surface (14.2).

**The session init (H-4c, 2026-09-05).** After the first tile's first
present the compositor runs ONE startup command as the user — the rc
(`$home/lib/halcyon.rc` under `ut --home`) or the device `default` layout's
restore — with the tile cap mask (`!CAP_SET_IDENTITY`), stdin from
`/dev/null`, stdout/stderr its own (the daemon log). `halcyond: session init:
<argv> (pid N)` / `session init exited (code N)` are the witnesses; a spawn
failure is said and the session lives on (an rc is a convenience, never a
gate). The pure decision is `halcyond::session_init` (host-tested); the rule
itself is §13.7's.
