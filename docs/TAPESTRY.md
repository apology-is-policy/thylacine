# Tapestry — a graphics fast-path woven on Loom

**Status: binding design — SIGNED OFF 2026-06-07.** The kernel/transport half
is the **Loom arc** (`docs/LOOM.md`); the graphics half (virtio-gpu scanout +
`tapestryd` + the SDL seam) is a **post-Loom graphics phase**. This document is
the canonical scripture; it is folded from the auxiliary-track design notes +
native proof-of-concept (`usr/apps/TAPESTRY-DESIGN.md` + `usr/apps/libtapestry`
+ `usr/apps/tapestry-demo`, authored on `aux/userspace-apps`), elevated here to
binding form with the Loom-arc integration made precise. The POC compiles
(the auxiliary track never boots); the kernel-side pieces are the main track's
to build.

Tapestry is the answer to "what is Loom *for*, beyond files?" — the concrete
consumer that shapes Loom-5 (multishot) and Loom-6 (registered buffers + the
native API), and the benchmark workload graphics imposes on both.

---

## 1. Naming

A **Loom** weaves threads into fabric; a **Tapestry** is the woven picture a loom
produces. The client *operates a Loom ring* (the io_uring-inverted 9P transport,
`docs/LOOM.md`) to present a **Tapestry** — a framebuffer surface. Loom is the
instrument; the Tapestry is what the client weaves onto the display. The display
server that owns the GPU and scans out tapestries is **`tapestryd`**. The lineage
is Plan 9 "I/O is messages" (the ring weaves messages) cashed in for pixels.

---

## 2. Thesis: graphics needs ZERO new Loom core

`docs/LOOM.md`'s central win over io_uring is "no new opcode namespace — 9P is
the vocabulary, uniform across files / `/net` / `/proc` / `/srv` / devices." A
framebuffer present is that uniformity cashed in once more:

- The **framebuffer** is a shared **Burrow** the client draws into directly
  (zero-copy — NOVEL.md Angle #2). It is **not** a Loom payload buffer: the host
  DMA-reads it during the server's `TRANSFER_TO_HOST_2D`, out of band, not over
  the ring.
- A **present** is `LOOM_OP_WRITE` of a small rect descriptor to a
  graphics-server fid. **No new opcode** (decision D3).
- **Input + vsync** are a **multishot `LOOM_OP_READ`** (Loom-5) on an event-fid:
  arm once, a CQE per event forever.

So **every multimedia fast-path — present, input, audio — is the same shape: a
9P server + Loom-5 multishot + Loom-6 registered buffers, with zero
media-specific kernel code, made safe across the untrusted-app -> trusted-server
boundary by I-29 / I-30.** That generality is the crown-jewel property: neither
io_uring (no capability model) nor a raw shared-memory ring (no safety) can claim
it. Graphics is the proof that Loom's generality + safety were worth their cost
— and the demonstration that NOVEL #1 (9P totalized) reaches the display, the
last subsystem Plan 9 itself never fully folded into the model.

**Performance.** The present's guest-side cost is an MMIO virtqueue kick + the
completion IRQ round-trip (`irq-bench` p99 < 5 us). Loom **hides that IRQ behind
the next frame's CPU rendering** via the async ring — present is
fire-and-forget + reap-later, not a per-frame stall. The one `tapestryd` context
switch per present (~1-5 us) is < 0.1% of a 4-17 ms frame and BUYS the
trust-boundary safety: a kernel-mediated I-30 pin stops an untrusted game from
TOCTOU-ing the rect descriptors or pointing scanout at arbitrary memory.

---

## 3. Architecture (native below, Pouch above)

```
  Pouch:   SDL  ->  SDL_thylacine video/input/audio backend  ->  libtapestry
  Native:  libtapestry (client weave; the POC) ==== Loom ring ====+
           tapestryd (owns virtio-gpu via CAP_HW_CREATE; the stratumd pattern):
              present-fid (Twrite rect) . event-fid (multishot read) . scanout
              ATTACH_BACKING the shared framebuffer Burrow -> virtio-gpu 2D resource
```

`tapestryd` owning the GPU is the **stratumd-as-driver precedent** (Phase 6,
sub-chunk 16b): the kernel stays out of the GPU (microkernel posture), the
device authority is a `CAP_HW_CREATE` grant to one trusted userspace server.
`libtapestry` is a **native** `libthyla-rs` library (the Plan 9 native/ported
split — `CLAUDE.md` "Native vs ported"); the SDL backend that ports Quake /
DOSBox sits **above** it via Pouch.

---

## 4. Resolved decisions (agreed with the user, 2026-06-07)

- **D1 — present CQE = "transfer-to-host done, buffer reusable"** (the recycle
  gate). Vsync is a **separate** multishot event for pacing. Decoupling lets a
  triple-buffered client render ahead without blocking and pace to the display
  independently of buffer recycling.
- **D2 — `tapestryd` creates the framebuffer Burrow.** The client asks for a
  W*H surface; the server allocates the Burrow + the virtio-gpu 2D resource and
  maps it back. GPU-resource authority stays with the owner (the A-1b
  capability-scoped-storage precedent: the resource holder mints, the client
  receives a handle).
- **D3 — present is generic `LOOM_OP_WRITE`** to a present-fid. Loom's opcode
  set stays **pure 9P**; there is no `LOOM_OP_PRESENT`. The rect descriptor is
  the write payload; the server interprets it.
- **D4 — damage = both** a single inline rect (the game case — zero extra
  buffer) AND a rect-list in a registered buffer (the compositor case). The
  inline form needs no Loom registered buffer; the list form rides Loom-6's
  registered-buffer surface.

---

## 5. The present / event protocol (the Loom op mapping)

| Client call | Loom op |
|---|---|
| `Display::new_tapestry(w, h)` | a control RPC to `tapestryd` (alloc the Burrow + the virtio 2D resource), then `SYS_LOOM_REGISTER` the present-fid + event-fid handles |
| `present(rect)` | `LOOM_OP_WRITE(present_fid, rect_descriptor)`; `user_data` = buffer id. The server does `TRANSFER_TO_HOST_2D(rect)` + `RESOURCE_FLUSH(rect)`; the reply -> a CQE (the recycle gate, D1) |
| `arm_events()` | a **multishot** `LOOM_OP_READ(event_fid)` (Loom-5); a CQE per input / vsync event; the bytes land in a registered event buffer (Loom-6) |
| reap / wait | drain the CQ; SQPOLL (Loom-4) makes submission syscall-free; `min_complete >= 1` waits (death-interruptible, #811) |

**Triple buffering.** Three sub-regions of the framebuffer Burrow; at most 3
presents in flight. The client paces naturally on buffer availability — stale
frames are dropped by **back-pressure** (the client reuses the oldest free
buffer), never by op cancellation. The present CQE (D1) is the
buffer-is-free signal; Vsync (a distinct event) is the display-pacing signal.
The POC's `Display::present` realizes exactly this: submit the present, then
reap until the next buffer is free (with triple buffering this almost never
blocks; it bounds in-flight presents to 2).

---

## 6. The one genuinely-new invariant — T-1, no torn scanout

**T-1 (no torn scanout): a present's framebuffer pages stay backed from submit
to its terminal CQE.** The host must never DMA-read freed/reused pages
mid-`TRANSFER_TO_HOST_2D`. This is the read-during-flight analog of I-29's
no-stale-completion, scoped to the *framebuffer Burrow* (which the host reads out
of band) rather than to a Loom CQE.

**T-1 is a Tapestry-layer invariant, not a Loom-core one** — the framebuffer is
a separate `tapestryd`-owned Burrow, not a Loom payload buffer, so Loom's I-30
pin (on the present-fid) does not by itself cover it. The mechanism already
exists:

- The **#847 dual-refcount** carries the heavy load: `tapestryd`'s mapping ref on
  the framebuffer Burrow keeps the pages alive independent of the client
  detaching or changing its namespace mid-flight.
- The **#898 quiesce-before-free** is the teardown model: a surface destroy /
  session death quiesces any in-flight present before the Burrow frees.

T-1 becomes a reserved ARCH §28 invariant + an audit obligation **when the
graphics phase lands** (virtio-gpu scanout + `tapestryd`), the way Loom deferred
its §28 I-29 / I-30 edit to its impl (`docs/LOOM.md` §9). No new kernel machinery
is owed for it — the pieces are present; the obligation is to *hold the ref
across the host-DMA window* and to audit that the present op's lifetime brackets
the `TRANSFER`.

---

## 7. What Loom-5 / Loom-6 must provide (the requirements graphics imposes)

This is the load-bearing fold into the Loom arc. The confirmed split
(user-voted 2026-06-07: "mechanism at Loom-5, real ops at Loom-6"):

- **Loom-5 — the multishot MECHANISM (one SQE -> many CQEs).** A persistent
  event stream that survives many CQEs and CQ back-pressure (coalescing is a
  *server* policy, not Loom's — `tapestryd` decides whether to merge input
  events, never the kernel). The generalization of I-29 to a stream:
  re-arm-after-each-completion; `LOOM_CQE_MORE` set on every non-terminal shot
  and cleared on the terminal one; the resources (the I-30 pin) held across all
  shots and released exactly once at the terminal CQE; back-pressure HOLDS a shot
  when the CQ is full (no lost, no double, no stale per shot); cancel/error
  yields exactly one terminal CQE. The `/srv` accept-loop and the event-fd are
  **the same mechanism** — one multishot op kind, two consumers.
  - Built + modeled + audited at Loom-5 against synthetic NOP/FSYNC multishot
    vehicles + the `specs/loom.tla` multishot actor (no real re-armable 9P op
    exists until Loom-6's payload surface). LINK/DRAIN per-fid ordering lands in
    the same sub-chunk (the ordering mechanism; its real dependent ops —
    walk->open->read, write->read — also light up at Loom-6).
- **Loom-6 — registered buffers + payload ops + the native API.** The event-fd
  multishot `LOOM_OP_READ` + the present `LOOM_OP_WRITE` (the real ops the
  mechanism rides) + a registered buffer for the rect-list (D4) and the event
  payload + the ergonomic `present` / `reap` wrapper that the libtapestry `Loom`
  seam trait targets (`impl tapestry::Loom for libthyla_rs::loom::Ring`). The
  **framebuffer itself is a separate server-shared Burrow, not a Loom regbuf**,
  so present imposes **no large-regbuf requirement** — only the small rect-list +
  event-payload buffers do.
- **Graphics is an explicit Loom-5/6 benchmark workload.** A present + input +
  vsync event loop is the canonical interactive case; the API is shaped to fit it
  (the POC's `Display` model is the shape the native API must support unchanged).

---

## 8. `tapestryd` (server) protocol sketch

A 9P service (the `/srv` post + the stratumd-as-driver `CAP_HW_CREATE` grant).
Per session:

- a **control fid** — `create-surface W H` -> a surface id + the framebuffer
  Burrow handle (D2); `destroy-surface`.
- a **present fid** per surface — `Twrite` of a rect (or a rect-list, D4) flushes
  it: `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`.
- an **event fid** — the multishot read source; `tapestryd` reads virtio-input,
  synthesizes Vsync from flush completion, and posts events.

---

## 9. The SDL seam (the on-ramp for Quake / DOSBox)

A Thylacine SDL **video backend** (`SDL_thylacine`) targets libtapestry:
`CreateWindowFramebuffer` -> `new_tapestry`; `UpdateWindowFramebuffer` ->
`present`; `PumpEvents` -> the multishot event stream; `SDL_audio` -> a future
audio server (the same shape; **no virtio-sound driver exists yet** — a
prerequisite for game audio). Software-rendered Quake / Chocolate-Doom / PrBoom
land on this; GZDoom stays gated on a GL stack (Mesa swrast via Pouch is the
realistic route, not a hand-rolled GL).

---

## 10. Unblock list (what must exist before this runs)

In dependency order:

1. **Loom-5 + Loom-6** — the multishot mechanism, then the native Loom API
   (`libthyla_rs::loom`) that implements the libtapestry `Loom` seam trait. (Loom
   is at ~4 today.)
2. **virtio-gpu scanout** — `CREATE_2D` / `ATTACH_BACKING` / `SET_SCANOUT` /
   `TRANSFER_TO_HOST_2D` / `RESOURCE_FLUSH` (the deferred half of
   `usr/virtio-gpu`; the controlq probe already exists).
3. **`tapestryd`** — the server above (`CAP_HW_CREATE`; the 9P protocol §8).
4. **virtio-sound** (for audio) — does not exist; scope alongside the audio
   server when game audio is in view.

Items 2-4 are the **post-Loom graphics phase** (NOVEL #2 + #4 territory;
Halcyon, the graphical shell, is the eventual consumer above `tapestryd`). The
Loom arc's obligation is to deliver item 1 shaped to fit (the §7 requirements).

---

## 11. The proof-of-concept (auxiliary track)

Built + compiling on `aux/userspace-apps` (`cargo build`; never booted):

- `usr/apps/libtapestry` — `Display` / `Tapestry` / `Event` / `Rect` + the
  `Loom` seam trait + `MockLoom`. The full client model (triple-buffer + the
  present recycle-gate + the multishot event drain) compiling against the
  documented future Loom surface. The handoff seam is one line: swap `MockLoom`
  for a `libthyla_rs::loom`-backed impl (Loom-6) with **zero** change to
  `Display` / `Tapestry`.
- `usr/apps/tapestry-demo` — an animated XOR-plasma software renderer (pure
  integer, no FP/libm) driving the model end to end. Compile-only; `MockLoom`
  completes presents immediately + schedules a `Close` so the offline run
  terminates. On real hardware the same source displays the live plasma. See
  `usr/apps/tapestry-demo/TEST-PLAN.md`.

When `aux/userspace-apps` merges, `usr/apps/TAPESTRY-DESIGN.md` remains the POC's
design notes; **this document is the binding scripture** it was folded into.

---

## 12. Sequencing + relation to the committed angles

- **Builds on NOVEL #2** (userspace drivers + BURROW zero-copy — the framebuffer
  is the zero-copy Burrow), **#3** (the pipelined 9P client — Loom is its
  userspace transport), and **#1** (9P totalized — present/input/audio are all
  9P). It is the graphics realization of **#4** (Halcyon): `tapestryd` is the
  display server Halcyon (Phase 10) eventually scans out through.
- **Shapes the Loom arc** (`docs/LOOM.md` §10): Loom-5 (multishot) and Loom-6
  (registered buffers + native API + bench) are designed to the §7 requirements;
  graphics is their benchmark workload.
- **Sequenced after Loom**, before/with the Halcyon graphical phase
  (`ROADMAP.md` §8.0a registers Loom; the graphics phase — virtio-gpu scanout +
  `tapestryd` — is post-Loom, NOVEL #2/#4 territory). It does not gate the
  shippable v1.0-rc (textual OS; `ROADMAP.md` §10): Tapestry is additive, like
  Halcyon.

---

> **Sections 13-17 added 2026-06-08** (design session; NOVEL/ARCH/ROADMAP
> companion commit). Sections 1-12 are the present/event *transport* (the Loom op
> mapping). Sections 13-17 are the *compositor + Halcyon UX + agentic enablement +
> sequencing* — the graphics phase elevated `tapestryd` from a present server to
> the compositor, and Halcyon's UI model evolved from the Phase-0 "scroll buffer
> as the sole primitive" to an anti-window tiling environment. This is
> **vision-sketch altitude** for Phase-10 detail (the §28 invariants reserve, like
> T-1 in §6, and land at impl); the *binding decisions* are mirrored into
> NOVEL.md (Angle #4 evolution + Angle #1 extension), ARCH §17, and ROADMAP.

## 13. The compositor — `tapestryd` elevated, and placement-transparent surfaces

**Decision (2026-06-08): `tapestryd` is not merely a present server; it is the
compositor / display server.** Sections 1-12 describe its present/event transport
(alloc a framebuffer Burrow, present a rect, stream events). The graphics phase
elevates it to own *layout* and *input routing* too, because Halcyon (Angle #4) is
an anti-window environment (section 14) and the compositor is what makes "inline
vs split vs tab" one mechanism. The monolith-vs-server fork resolves to
server+clients: it is the Plan 9 way (the window system is a file server,
section 15), the robust way (a fullscreen Quake crash must not take the desktop),
and it makes placement-transparency fall out.

- **D5 — surfaces are placement-transparent.** A client is handed a surface (a
  framebuffer Burrow + a present-fid + an event-fid) and an input channel. It does
  **not** know, and cannot observe, whether the compositor draws it inline in a
  scroll buffer, in a split pane, as a fullscreen tab, or docked in a corner.
  Placement is the compositor's business. This collapses Halcyon's display modes
  (section 14) into one mechanism, and it is why an SDL/Quake client (section 9)
  "just made a window" while Halcyon places it as a tab.
- **D6 — surfaces re-parent live.** Because placement is invisible to the client,
  the compositor moves a *running* surface between placement states without the
  client restarting or knowing. Start a game inline as a thumbnail, promote it to a
  fullscreen tab; the client renders the same surface throughout. Live promotion
  (section 14) is an emergent property of D5, not a separate feature.
- **D7 — the compositor stays a pure surface + pixels + input router.** It deals in
  framebuffer Burrows and input streams, never glyphs, fonts, or text. Text
  rasterization (TTF; section 14) and modal editing (the Helix transcript) are
  *client* concerns — Halcyon rasterizes its own panes; a non-text client (Quake)
  never drags in the font stack. This is the Wayland-style render-in-the-client
  discipline; it keeps the compositor protocol minimal.

The two transports stay split (present-on-Loom / control-on-9P, section 15):
pixels fly over the Loom ring (section 5) for latency; structure and input are 9P.
`tapestryd` owning the GPU remains the stratumd-as-driver `CAP_HW_CREATE`
precedent (section 3).

## 14. Halcyon — the anti-window environment (the compositor's first client)

**This section EVOLVES NOVEL.md Angle #4** (2026-06-08). The Phase-0 model was "a
scroll buffer with inline graphics as the sole UI primitive." That insight is
*preserved and subsumed*: a Halcyon shell pane **is** a scroll buffer whose
transcript can hold inline graphical surfaces. The evolution adds the layer
*around* the pane — a tiling, anti-window compositor — turning "the scroll buffer
is the UI" into "the terminal is the desktop."

**Thesis: the floating, overlapping window is obsolete; the terminal is the
desktop.** Grounding: Plan 9's acme (text-first tiled columns, "run anything in a
pane") + `/dev/draw`-as-a-file surface multiplexing; the modern tiling-WM
convergence (i3 / sway / zellij); and the mainstream's own concession — 40 years
of overlap-*management* UI (taskbars, alt-tab, Expose, virtual desktops, now
Windows 11 Snap Layouts + macOS Sequoia tiling) are all workarounds for a problem
tiling deletes. Most work happens in one or two surfaces at a time; a tiling split
handles that without windows. Heritage and SOTA both point here.

The model:

- **Uniform container, layout-mode per container (the i3 model).** One structural
  primitive: a container, whose layout mode is `split-h | split-v | tabbed |
  stacked`. "Tabs" and "splits" are not separate levels — they are layout modes of
  the same container, nestable arbitrarily. The screen is the root container; a
  taskbar is simply `root = vsplit[ working-area(tabbed), status-bar(thin) ]`.
- **Placement states (all D6 live-reparent transitions):** `{ inline-live,
  pinned(scope, policy), pane-filling, pane-zoomed, tab-filling }`. "Fullscreen" is
  pane-zoom (fill the split pane, tmux-`zoom` style) or tab-fill.
- **pin = minimize = widget, one mechanism.** A pinned surface is a running surface
  docked out of the flow. A widget (a clock) is *born* pinned by `halcyon.rc`; a
  minimized app is *promoted* to pinned. Two parameters distinguish the cases:
  `scope` (pane | screen — a clock docks to a screen-level chrome pane so it
  survives a pane close and does not multiply per-split; a minimized app docks to
  its origin pane) and `policy` (overlay | reserve-strip — a clock reserves a
  status strip; a minimized video overlays a corner). Multiple pins tile into a
  corner dock / a designated pin-target pane.
- **Mechanism / policy split.** Panes are one primitive; the special-ness — `role`
  (content | chrome | pin-target), `focusable` (false for chrome so Super+arrows
  skips the clock) — is `halcyon.rc` *policy*, not new primitives. This is what
  lets "everything is a pane" hold up without twelve widget types. Pure Plan 9
  mechanism/policy discipline.
- **Inline-surface lifetime is resolved by the focus boundary.** While an inline
  app is focused you cannot scroll the shell (focus is in the app); handing control
  back resolves the surface to *close* or *pin* immediately. So a live inline rect
  never strands "scrolled half off-screen" — app-focused and shell-scrolling are
  mutually exclusive, with close/pin as the transition.

Input is two orthogonal layers:

- **Layer 1 — compositor control, a reserved Super/Hyper modifier (the i3/sway
  model).** Super+arrows move focus, Super+N switches tab, Super+m minimizes —
  addressable *always*, even through a greedy app (Quake, vim) that eats every
  other key. This is how you talk to the compositor without a tmux-style prefix.
- **Layer 2 — the focused surface's own input.** For a shell pane it is a
  **Helix-flavored modal transcript** (selection-first matches the copy-centric
  nature of terminal work): Esc -> normal mode, navigate/select/`y`ank anywhere in
  the (read-only) scrollback, `i` jumps to the (only writable) prompt. Navigation +
  selection + yank are global; insert/paste/edit resolve to the prompt. A free gift
  falls out: select any past command, tweak, resubmit — acme's "text is
  executable," modal. For an app client, layer 2 is raw input. Mouse is secondary
  (selection, focus, layout drag); keyboard is primary.

**TTF is foundational, not a nicety** (the Phase-0 fontdue note becomes
load-bearing). A native `no_std` TrueType rasterizer (fontdue / swash-class;
aux-forkable like ratatui->nora) with AA + hinting; complex-script shaping (CJK /
ligatures / RTL; HarfBuzz-class) is deferred — a monospace Latin + box-drawing
terminal does not need it initially.

**The eyes-open cost.** No-floating-window is a deliberate break. Terminals, TUIs,
and single-surface games map perfectly; multi-window / popup toolkit apps (GTK/Qt
menus, tooltips, dialogs, multiple top-levels) map awkwardly — a menu is a
transient floating window by nature. Accepted: the porting target is games / TUIs,
not the Linux desktop-app ecosystem.

## 15. Layout-as-9P — the compositor is a file server

**Decision (2026-06-08): the compositor exposes its layout as a 9P tree** (the
maximal acme realization — acme exposes each window as files `ctl`/`body`/`tag`/
`addr`; rio serves `/dev/draw`, `/dev/cons`, `/dev/mouse`). `halcyon.rc` is then
*a shell script writing files*, not a config DSL; a program reads its own geometry,
requests promotion, or pins itself by file ops — the identical interface a human
uses, and the same interface the agent uses (section 16). Present-on-Loom /
control-on-9P made concrete: pixels over the Loom ring, structure over 9P.

Tree sketch (names provisional):

```
/dev/halcyon/
  ctl                 # global: new-tab, focus <id>, ...
  layout              # read: the container tree as text; write: mutate
  event               # read: a stream of compositor events (focus, resize, retitle)
  pane/
    new               # read/write: allocate a container, yields its <id>
    <id>/
      ctl             # split, close, set-mode, promote-fullscreen, pin
      mode            # split-h | split-v | tabbed | stacked
      role            # content | chrome | pin-target   (+ focusable bit)
      geometry        # read: x,y,w,h px (and rows,cols for a text pane)
      tag             # acme-style title / command line
      surface         # read: a Loom present-handle for this pane's pixels
      input           # read (when focused): keystrokes + mouse for this pane
```

The three planes that *are* the architecture: **structure/control -> 9P** (this
tree; scriptable, introspectable), **pixels -> Loom** (`surface` yields a
present-handle; the firehose never touches the filesystem), **input -> the
per-pane `input` stream** (focus-routed, with the Super-chord layer 1 intercepted
above it). Every hard mechanism lives on exactly one plane — the test that the
layering is clean.

Forward-compat: **surfaces carry an alpha channel (RGBA) from day one** even
though blending starts at 1.0 — adding alpha later is an ABI break on the hot
path. Translucent chrome (a clock over scrollback) is the eventual payoff; content
apps (Quake) never want it, so transparency pairs with the chrome/widget role.

## 16. Agentic enablement — the graphical agentic-loop ABI

**Decision (2026-06-08): the graphics phase ships a perceive / act / assert API
for the agent, designed in from the fbcon (stage 0, section 17), not bolted on.**
Load-bearing for the *development methodology*: every existing agentic loop is
textual (`Thylacine boot OK`, `tests: N/M PASS`, `EXTINCTION:`); a compositor
emits pixels, so without this the post-Utopia agent-primary model regresses to
human-primary exactly when the work turns visual. It is a new agentic-loop ABI,
sibling to the boot-banner ABI (`TOOLING.md` §10), which gains the concrete
contract when the fbcon lands.

- **Structural perception = free** — the agent is just another 9P client of the
  section-15 layout tree; it `cat`s `/dev/halcyon/layout` over the serial console
  it already drives. No new mechanism.
- **Visual perception, two planes.** Host-side, immediate: QEMU `screendump` over
  QMP writes the scanout to a PNG the agent reads *visually* (the Read tool ingests
  images) — works the instant virtio-gpu scans out anything. In-band, later: a 9P
  `screen` / `pane/<id>/snapshot` the compositor renders to an image (per-pane,
  structurally correlated, works on real hardware).
- **The oracle / ground-truth pairing is why both are needed**, not two loose
  features. The 9P tree says what *should* be on screen; the screendump shows what
  *is*. Structure alone can lie about whether something rendered; pixels alone have
  no oracle. Cross-checking the two makes graphical agentic testing rigorous.
- **Action** mirrors it: QMP `input-send-event` host-side (prior art: the #896
  virtio-input key-injection harness) + an in-band inject file later.
- **Determinism is the hard part** (not capture). Vsync timing, cursor blink, the
  ticking clock, glyph jitter make golden-image assertions flaky (the project
  already feels the milder version at #887 / #896 / #894). Required hooks: a
  *hold-this-frame* present mode, a test mode that freezes animations + cursor
  blink, deterministic glyph rendering.
- **Gate the in-band capture / inject files to dev/test builds** — a screen
  readback or input-injection file is screenshot-spyware / control-hijack in
  production (the strip-for-production class of #880). The host-side QMP path needs
  no guest gating (it is the host's own capability).

## 17. Sequencing, tiers, and the two axes

**The investment ramps; it does not switch on.** Design (this doc) matures cheaply
through Phase 7-8; the build is staged so the API is proven by real-but-simple
consumers before Halcyon — the highest-risk, last-phase client — commits to it.

- **fbcon = Tapestry stage 0.** The shell on a real monitor: the compositor with
  exactly one fullscreen surface (a shell pane). It is the forcing function for the
  bottom of the stack (scanout + raster + bitmap text + input), the first real
  consumer that hardens the green virtio-gpu / virtio-input drivers, and the one
  graphics piece with a v1.0-rc claim ("a real OS on a monitor, not just a serial
  console"). The agentic capture/inject loop (section 16) wires in *here*, so
  graphics is never developed blind — and it is the first time the agent literally
  `Read`s a screenshot of Thylacine. Late Phase 9 (`ROADMAP.md`).
- **Compositor API + SDL / software-Quake = the acceptance gate, BEFORE Halcyon.**
  Grow the protocol (pane-tree, placement-transparency, re-parenting) and prove it
  under a demanding non-Halcyon client. Original Quake shipped a *software*
  rasterizer — software-Quake is a 2D milestone gated only on the SDL->Tapestry
  backend (section 9), no GL. If SDL/Quake maps cleanly, the API is proven *before*
  the riskiest client is built. Phase 10 on-ramp.
- **Halcyon = the headline client** on the now-proven protocol. Phase 10.
- **Two tiers, and Halcyon is pure 2D.** A textual-heritage graphical shell (scroll
  buffer, image display, video) is decode-then-blit — Halcyon never needs OpenGL.
  **GL is exclusively for ported third-party apps** (off Halcyon's critical path),
  and the system-optimal route is *port Mesa* (swrast / llvmpipe via Pouch), not a
  hand-rolled GL; a native GL pipeline is a NOVEL research angle, not a roadmap
  dependency. GL is v1.1+ and never blocks Halcyon.
- **Two axes — decouple them.** (A) the Tapestry API stack (compositor -> present
  -> driver -> scanout) is entirely QEMU-validatable (virtio-gpu + virtio-input) —
  build and perfect the whole API + fbcon + Halcyon there. (B) bare-metal
  enablement is Lazarus work (`PORTABILITY.md`) that plugs in *under* the finished
  API: the RPi framebuffer (output, moderate) and — the long pole — **USB-HID for
  the keyboard** (a USB host stack, one of the largest driver subsystems).
  "Operable on bare metal through keyboard + monitor" is the A∩B intersection; the
  input half is gated on USB, so build QEMU-first (virtio-input) and let the
  bare-metal backends land independently on Lazarus.

---

## Cross-references

- `docs/LOOM.md` — the ring transport Tapestry operates (§8 the ABI, §9
  I-29/I-30, §10 the sub-chunk decomposition Tapestry shapes).
- `docs/NOVEL.md` §3.2 (Angle #2, BURROW zero-copy drivers) + §3.3 (Angle #3,
  pipelined 9P client) + §3.4 (Angle #4, Halcyon).
- `usr/apps/TAPESTRY-DESIGN.md` + `usr/apps/libtapestry` + `usr/apps/tapestry-demo`
  (the auxiliary-track design notes + POC this scripture was folded from).
- `usr/virtio-gpu` (the controlq probe; the scanout half is the graphics-phase
  unblock item) + `irq-bench` (the present IRQ-cost budget).
- `ARCHITECTURE.md` §28 (where T-1 reserves a number when the graphics phase
  lands) + §25.4 (the audit-trigger surface `tapestryd` joins then).
