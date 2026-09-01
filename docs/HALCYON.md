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
- **The universal-floor question (open, measurement-gated)**: the local macOS
  dev loop and mode-1 "Thylacine ships with QEMU" deployments have **no venus**
  (the host cannot offer blob+hostmem). The candidate answer is **lavapipe
  inside the guest** — vk-on-CPU as a second ICD, one renderer code path, no
  host GPU dependency; plausible now because llvmpipe needs LLVM JIT + threads
  and the Clade arc landed both (CL-7k). This is an investigation chunk with a
  measurement gate, not an assumption. Until it lands, Halcyon requires a
  GPU-capable lane and **Aurora covers every other deployment** — the fallback
  posture (VISION §3.3) is doing its job, not being violated.

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
  taken); guest-lavapipe (the §2 investigation, schedulable any time).
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

## 11. Sequencing (provisional chunk families — the operator sequences)

- **H-0** — this scripture (+ BEACON.md; the ROADMAP/VISION/NOVEL/TAPESTRY
  reconciliation).
- **H-1 — Beacon foundations**: `SYS_FD_DEVCLASS` (pulled forward; unparks
  `--color=auto` too) → the `libthyla-rs` beacon module (cells realization
  relocated, verbatim behavior) → ut zones → first emitters (ls, grep, ps,
  stat). Useful to Aurora/serial users immediately; no Halcyon binary needed
  to land value.
- **H-2 — the vk pane renderer**: glyph atlas + TTF rasterizer + DejaVu; the
  rich transcript MVP (zones → blocks; plain VT fallback rendering); the
  paper-light theme.
- **H-3 — presentations**: `obj` rendering, the verbs rules engine, context
  menus (compositor-placed chrome), the executable tag bar.
- **H-4 — layouts** (save/reload/respawn) + `halcyon.rc`.
- **H-5 — compose** (§10) and, behind it, the parked display-wall ledger.
- **H-6 — the guest-lavapipe investigation** (schedulable anywhere; gates the
  universal floor claim, nothing else).
- **H-7 — integration + audit**: Aurora-as-panes polish, image display, the
  video decision, the Halcyon-surface audit round, `docs/HALCYON` manual
  chapter, the ROADMAP §11.2 exit criteria pass.

## 12. Naming rationale + status

**Halcyon** (locked since Phase 0) — the calm day; the impossible return. The
light family it anchors: **Aurora** the dawn, **Halcyon** the day, **Bonfire**
the palette, **Beacon** the signal (BEACON.md §11). The paper-light default
(§3) makes the pairing literal.

- **2026-09-01**: scripture adopted — the Halcyon kickoff design conversation
  (operator vision: i3 + acme + Genera; all forks resolved in-session; Beacon
  named). Supersedes the Phase-0 §11.1 deliverables and the pure-2D posture.
  No code. H-1 is the natural opening chunk.
