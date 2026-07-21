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

> **Section 18 added 2026-07-17** (the graphics-phase design pass — the pass
> ROADMAP §11 forecast ("the §11.1 bullets firm up at the graphics-phase design
> pass"). Sections 13-17 were vision-sketch altitude; §18 is the BINDING
> concretization: the surface lifecycle, the present/recycle + resize protocol,
> the event wire, the `/dev/tapestry` schema, the trusted-path + `/dev/cons`
> reconciliations, T-1 formalized + spec-gated, and the G-0..G-9 build arc.
> Grounded in a full as-built substrate survey (2026-07-17) — every "exists /
> does not exist" claim below is verified against the tree, not assumed.

## 18. The graphics-phase concretization (design pass, 2026-07-17)

### 18.0 Votes recorded + ground truth

**Four user votes (2026-07-17) bind this section:**

- **V1 — tapestryd-minimal from day 0.** Stage 0 (fbcon/Aurora bring-up) stands
  up `tapestryd` as the GPU owner with exactly one fullscreen surface; Aurora is
  its first client over the REAL surface protocol. No interim
  Aurora-owns-scanout state, no later migration; the protocol is exercised from
  the first pixel (the §17 prove-before-Halcyon intent realized).
- **V2 — surface mapping generalizes Weft grant-is-the-share.** Holding the
  surface's weave fid IS the capability; the client maps via the Tweft-class op
  on that fid; no Burrow handle ever crosses Procs (the I-4 posture preserved;
  the free-floating-handle class the Weft arc rejected stays rejected).
- **V3 — spec-first RE-ENABLED for T-1** (the 6th instance of re-enable point
  (a)): `specs/tapestry_present.tla` is written TLC-green BEFORE the tapestryd
  impl (§18.8).
- **V4 — the compositor tree mounts at `/dev/tapestry`.** The tree belongs to
  the compositor; Halcyon is one client of it (as are Aurora terminals, an
  SDL/Quake surface, and the agent). This SUPERSEDES §15's provisional
  `/dev/halcyon` naming; `halcyon.rc` is a script writing `/dev/tapestry`
  files. An Aurora-only install mounts a correctly-named tree.

**Ground truth (as-built survey, 2026-07-17)** — what the design stands on:

- `usr/virtio-gpu` is NOT probe-only (its Cargo comment is stale): it already
  drives the FULL 2D pipeline — `GET_DISPLAY_INFO` / `RESOURCE_CREATE_2D` /
  `RESOURCE_ATTACH_BACKING` / `SET_SCANOUT` / `TRANSFER_TO_HOST_2D` /
  `RESOURCE_FLUSH` — rendering a test pattern once, then exiting. One-shot,
  virtio-mmio transport, spawned only by the kernel test harness. The scanout
  bottom is a PROMOTION (resident + loop), not a greenfield.
- The native Loom API (`libthyla_rs::loom`) is complete for the §5 mapping:
  `MULTISHOT` + `LINK` reachable (`Sqe::with_flags`), opcodes 9P-only as
  designed. Zero new Loom core confirmed again.
- Cross-Proc sharing: `burrow_share_into` exists but is **ANON-only** (rejects
  MMIO/DMA types); `SYS_WEFT_SHARE` is CAP_HW_CREATE-gated (tapestryd holds it
  anyway); `SYS_WEFT_MAP` resolves any dev9p `KOBJ_SPOOR` fid and issues
  `Tweft` — the mechanism is server-agnostic in shape, netd-only in use. The
  generalization is a naming/validation pass + the §18.1 backing decision + a
  bounded amount of NEW machinery (the framebuffer-weave map branch and the
  retire-time disarm — §18.11 F3/F10 CORRECT the earlier "not new machinery"
  claim: `SYS_WEFT_MAP` is idempotent-per-fid and there is no
  `weft_share_unregister` today, so the arm/re-arm/disarm weave lifecycle needs
  real additions, scoped into the G-2 pass).
- NO framebuffer surface exists anywhere (no `/dev/fb`, no fb Dev, no
  tapestryd binary); virtio-input events reach only the boot log. ~~The
  warden's `virtio:16` (GPU) slot is currently bound to the `crash-probe`
  restart test; no screendump is wired~~ — BOTH retired: G-0 landed
  `tools/screendump.sh` (the §16 agentic-eyes capture, headless-proven) and
  G-1 landed the resident `gpud` on `virtio-pci:16` with crash-probe
  re-homed to the synthetic `restart-test` node (see the G-1 AS-BUILT note
  under §18.9).

### 18.1 The surface lifecycle — create, weave, present, resize, retire

A surface's life is a strict state machine (modeled in §18.8):

```
create-surface ──> WOVEN (weave allocated, mapped by client via Tweft)
   WOVEN ──ready──> LIVE (presents flowing; at most N-1 of N slots in flight)
   LIVE ──configure/resize-ack──> REWEAVE (new weave allocated; old draining)
   REWEAVE ──first present into new weave──> LIVE (old weave retires after quiesce)
   any ──destroy / clunk / client death──> RETIRING (quiesce in-flight) ──> gone
```

- **Create**: `surface/new` (9P) yields `<id>`; `create W H` on the surface
  `ctl`. tapestryd allocates the weave (the framebuffer Burrow, D2) + the
  virtio-gpu 2D resource, and registers the weave kernel-side
  (`SYS_WEFT_SHARE`-class).
- **Map (the V2 realization)**: the client opens `surface/<id>/weave` and calls
  the generalized map syscall on that fd; the kernel issues `Tweft(F)` to
  tapestryd over the mounted session, joins the returned `share_id` to the
  registered Burrow, and `burrow_share_into`s it RW (never X — W^X). Teardown
  inherits `ShareBoundedByFlow`: the weave fid's clunk drops the client
  mapping; T-1 handles the server side (§18.8).
- **The backing decision (the ONE kernel delta beyond syscall genericity).**
  The weave must be BOTH client-mappable AND GPU-attachable
  (`ATTACH_BACKING` needs stable guest-physical addresses, which is exactly
  what `t_dma_create` mints). But `burrow_share_into` is ANON-only today. Two
  candidate resolutions, decided here:
  - REJECTED — client draws into an ANON weave and tapestryd copies into its
    DMA backing per present: a full-frame memcpy on the hot path kills the
    Angle-#2 zero-copy property Tapestry exists to demonstrate.
  - **BOUND — admit a NARROWED device-passive DMA weave subtype into
    `burrow_share_into` (and the Weft claim path); NOT `BURROW_TYPE_DMA`
    generally** (§18.11 F1 tightens this — the earlier broad-DMA phrasing
    weakened I-5 from a *structural* guarantee to a *server-behavioral* one).
    A weave is allocated as a distinguished **kernel-tracked** weave-backing
    Burrow type (round-2 R2-F1 corrects the "structural like MMIO" claim: a
    creator-asserted *flag* is NOT kernel-verifiable — device-passive-ness is a
    sub-distinction WITHIN the DMA type that the kernel cannot derive from the
    physical resource the way it derives MMIO-vs-DMA, so a bare flag would rest
    on the creator's honesty, one layer down). The structural guarantee holds
    ONLY if the kernel MINTS the type and BINDS its use: a `BURROW_TYPE_GPU_BACKING`
    (name provisional) the kernel hands ONLY to the `RESOURCE_ATTACH_BACKING`
    scanout path, NEVER to a virtqueue/descriptor region the device reads as
    commands, and the share path admits ONLY that type — then a device-command
    DMA region is structurally unshareable exactly as MMIO is. A weave-backing
    Burrow is pinned RAM the device only DMA-*reads* (pixels); a client's
    Normal-WB RW mapping of it (the same cacheable attrs as ANON — distinct from
    MMIO's Device-nGnRnE) conveys ZERO hardware authority. **This is an ABI
    addition requiring user signoff at G-2** (`SYS_DMA_CREATE`/`t_dma_create` is
    two-arg today — a new type/flag or a new allocation syscall is
    escalation-worthy per CLAUDE.md); until it lands, the honest fallback is that
    the guarantee rests on tapestryd's EXISTING trust as the sole GPU owner (the
    same trust it already holds by owning the device — not a NEW trust, but not
    MMIO-parity structural either). Prosecute at G-2 (the DMA case is a distinct,
    currently-unaudited hardware-mapping surface): the KObj_DMA second-refcount
    domain + the virtio-gpu-2D-resource detach are NOT "type-independent" with
    ANON (a DMA Burrow has `pages == NULL`, its refs live on the KObj_DMA) —
    fold both into the T-1 spec/audit rather than asserting equivalence.
    (§18.12 R2-F1.)
    **G-2 AS-BUILT (2026-07-19; the ABI user-signed-off 2026-07-19 in advance —
    the recorded pre-signoff, scoped to exactly this item):** the mint is a NEW
    SYSCALL `SYS_DMA_CREATE_WEAVE` (99; a flags widening of the two-arg
    `SYS_DMA_CREATE` was rejected — existing callers leave x2 as garbage, and a
    garbage flags word could accidentally set share-admissibility, the #112
    class), setting the create-immutable `KObj_DMA.weave` bit (the
    `BURROW_TYPE_GPU_BACKING` provisional name resolved to a kobj-level subtype
    — zero switch-site churn, identical structural force); the F3 disarm is
    `SYS_WEFT_UNSHARE` (100; also closes the netd #289 seam); the R2-F3 budget
    is `Proc.shared_map_pages` (128 MiB — the I-32 fifth axis). T-1 is
    ENUMERATED as **I-40** (ARCH §28) with the kernel share half ENFORCED; the
    present half completes the row at G-3. As-built:
    `docs/reference/125-weft.md` "The weave share (G-2)"; the action ↔ site
    map: `specs/SPEC-TO-CODE.md::tapestry_present.tla`.
- **Slots**: one weave carries N=3 page-aligned slot sub-regions (D1 triple
  buffering); tapestryd chooses stride/offsets and reports them in `geometry`.
  The client draws only into free slots; the present CQE frees a slot (D1).

### 18.2 The present protocol (concretizing §5)

- Present = `LOOM_OP_WRITE` on `surface/<id>/present` (D3; zero new opcodes —
  re-verified against the as-built opcode table). The write payload is a
  versioned, `_Static_assert`-pinned descriptor:

```
struct tpresent {            /* little-endian, 32 bytes, version pinned */
    u32 version;             /* TPRESENT_V1 */
    u32 slot;                /* which weave slot completed drawing */
    u32 flags;               /* TPRESENT_HOLD (§18.6 determinism), ... */
    u32 rect_count;          /* 0 = full-surface damage */
    struct trect { u32 x, y, w, h; } rect0;   /* inline rect (D4 game case) */
    /* rect_count > 1: rects live in a registered-buffer slice named by
       buf_idx_or_off (D4 compositor case) */
};
```

- Semantics: tapestryd validates slot + rects against the surface geometry
  (reject out-of-bounds — the untrusted-client boundary), then
  `TRANSFER_TO_HOST_2D(rects)` + `RESOURCE_FLUSH`; the reply posts the CQE.
  **The CQE is the recycle gate (D1): slot reusable, nothing more.** Presents
  on one surface complete in submission order (the server processes a
  present-fid FIFO; `LINK` is unnecessary for the common loop).
- Back-pressure: the client self-paces on free slots (at most N-1 in flight);
  no cancellation path on the hot loop — stale frames are dropped by reuse
  (§5), never by op cancel.

### 18.3 Resize — the new-weave protocol (never realloc-in-place)

Placement-transparency (D5) makes resize COMPOSITOR-initiated. The dance is
Wayland-xdg-shell-class (configure/ack, serialized), realized Thylacine-shaped:

1. tapestryd emits `CONFIGURE {serial, W, H}` on the surface's event stream
   (the client learns sizes ONLY this way — it cannot ask for placement).
2. The client acks: `resize W H <serial>` on `ctl`. tapestryd allocates a NEW
   weave + resource at the new size (state REWEAVE). The client obtains the new
   mapping by **re-opening `surface/<id>/weave` (a FRESH fid per generation)**
   and calling the map syscall on it — NOT by re-calling map on the old fid
   (§18.11 F3 CORRECTS the earlier "same fid" prose: `SYS_WEFT_MAP` is
   idempotent-per-fid and would return the old weave's cached VA). The fresh
   fid carries no stale binding cache; the old weave fid is clunked as its
   presents drain. The `resize`-ack `Rwrite` reply completes only AFTER g2 is
   allocated (§18.12 R2-F5), so the client's fresh-fid re-open is guaranteed to
   bind g2 and never resolve to g1.
3. The client's first present into the new weave **completes** (its terminal
   CQE — the first frame has transferred) and tapestryd then switches scanout
   composition (`SET_SCANOUT`) to it (§18.11 F16 aligns this with the spec's
   switch-at-Complete: you do not `SET_SCANOUT` a resource before its first
   frame lands). The OLD weave retires only after its in-flight presents
   quiesce (#898 discipline; T-1). Until the switch, tapestryd shows the old
   content letterboxed/scaled/frozen — compositor policy, invisible to the
   client.
4. **Never realloc-in-place.** A weave's page membership is immutable for its
   lifetime — this is what keeps T-1's reasoning (and the spec's state space)
   small. Unacked CONFIGUREs coalesce; only the latest serial matters.
   **At most one reweave is in flight per surface**: a new reweave may not
   begin until the prior old weave has FULLY retired (§18.11 F6 — this
   serialization is what bounds a surface to ≤2 live weave generations, the
   bound the spec models; a resize burst queues, it does not stack
   generations).

### 18.4 The event stream — one wire, fixed records

One multishot `LOOM_OP_READ` on `surface/<id>/event` (Loom-5; `CQE_MORE`),
payload = fixed **24-byte** records into a registered event buffer:

```
struct tevent {              /* little-endian, 24 bytes, version-pinned wire */
    u16 kind;                /* KEY, PTR_MOVE, PTR_BTN, SCROLL, FRAME,
                                CONFIGURE, FOCUS, CLOSE, PTR_REL */
    u16 code;                /* KEY: evdev keycode (virtio-input passthrough);
                                PTR_BTN: button; CONFIGURE: low bits of serial */
    u32 value;               /* KEY: press/release/repeat; PTR: packed x<<16|y
                                (surface-RELATIVE); PTR_REL: packed SIGNED
                                display-pixel deltas dx<<16|dy (i16 each);
                                CONFIGURE: W<<16|H */
    u32 rune;                /* KEY: compositor-resolved UTF-32, 0 if none */
    u16 mods;                /* modifier bitmask (shift/ctrl/alt/super/...) */
    u16 flags;
    u64 tick;                /* the display-clock tick of delivery */
};
```

- **The compositor owns the keymap** (anti-Wayland-xkb decision): every KEY
  event carries BOTH the raw code (games) and the resolved rune (shells,
  Plan 9-style). No keymap hand-off protocol, no client-side xkb. Layouts are
  a tapestryd config concern.
- **Pointer coords are surface-relative** — absolute screen coords would leak
  placement through the D5 wall.
- **PTR_REL is the mouse-look stream** (the relative-mouse arc): every
  pointer motion reaches the FOCUSED surface as signed display-pixel deltas
  (a focus companion like KEY — decoupled from the pointer position;
  PTR_MOVE keeps the under-pointer rule). Deltas are EXACT from a relative
  device (virtio-mouse) and SYNTHESIZED from consecutive absolute motion —
  load-bearing under abs-only frontends (QEMU cocoa with a tablet present
  never produces host rel events; the edge-stall at the host window
  boundary is inherent to the abs source). Deltas leak no placement (pure
  motion — D5-consistent). Queueing: back-of-queue REL records coalesce by
  SUMMATION (replacement would lose motion; an interleaved event starts a
  fresh record, preserving order) and the kind is droppable under stall
  (lossy motion stream — a burst must never WEDGE a client). A relative-mode
  client (SDL mouse-look) consumes PTR_REL and must NOT diff successive
  PTR_MOVE positions (every motion emits both — a diff double-counts).
- **FRAME is the display clock** (D1's pacing signal): base virtio-gpu 2D has
  NO guest-visible vblank (`EVENT_DISPLAY` is config-change only — verified),
  so tapestryd SYNTHESIZES a fixed-rate FRAME tick (60 Hz default,
  `clock-rate` ctl). Honest by construction; a real vblank source on future
  hardware backends slots in behind the same event. A client that only
  recycles on present-CQEs never needs FRAME (the game loop); a client that
  paces (video) arms it.
- The Super/Hyper compositor-control layer (§14) is intercepted ABOVE this
  stream: reserved chords never reach a surface's events.

### 18.5 The `/dev/tapestry` tree (V4 — supersedes §15's provisional naming)

```
/dev/tapestry/
  ctl                  # global: test-mode on|off, clock-rate <hz>, ...
  layout               # the container tree (compositor stage, G-6): read text,
                       #   write mutations — §15 semantics, renamed
  surface/
    new                # read: allocate a surface, yields <id>
    <id>/
      ctl              # create W H | resize W H <serial> | title ... | destroy
      weave            # the map-capability fid (V2 Tweft target);
                       #   read: geometry text (W H stride slots fmt)
      present          # the Loom present fid (LOOM_OP_WRITE of tpresent)
      event            # the multishot event fid (tevent stream)
      geometry         # read: x,y,w,h + rows,cols (text panes)
  pane/                # the LAYOUT layer over surfaces (compositor stage,
    ...                #   G-6): §15's pane/<id>/{ctl,mode,role,tag,...},
                       #   with pane/<id>/surface naming the surface it hosts
```

Stage 0 (V1) ships ONLY `ctl` + `surface/` with one fullscreen surface; the
`pane/` + `layout` layer arrives at G-6 (the §15 schema otherwise stands).
"weave" as the map-fid name is the thematic proposal of record (the woven
picture a client maps and draws into); `fb` is the fallback if it reads as
obscuring. The tree is served by tapestryd over `/srv` + dev9p, mounted at
`/dev/tapestry` by the boot chain (the /net mount precedent).

### 18.6 Determinism mode (the §16 wire, made concrete)

`test-mode on` (global ctl; **dev/test builds only** — the #880
strip-for-production class, enforced at build time, not runtime):

- freezes the FRAME clock (ticks only on `tick` ctl writes — the test drives
  time), suppresses cursor blink + animation timers tapestryd owns, and
- `TPRESENT_HOLD` presents complete normally but scanout composition is
  deferred until `release` — the hold-this-frame primitive for golden-image
  capture.
- The capture pairing stays as §16: QMP `screendump` (host-side, works with
  `-display none` — G-0 verifies this FIRST, before anything builds on it) vs
  the `/dev/tapestry` structural view; assert both, cross-checked.

### 18.7 Trusted-path + `/dev/cons` reconciliations

- **Episodes bind to kernel-reachable framebuffers only.** TRUSTED-PATH's
  strong model ("during a framebuffer episode NO userspace maps the
  framebuffer; the kernel is the sole painter") requires the kernel to paint
  WITHOUT the userspace GPU owner — possible on a linear simplefb-class
  medium (kernel maps the firmware buffer), impossible on virtio-gpu without
  a kernel virtio-gpu driver (rejected — ARCH §17.2's no-graphics-in-kernel
  holds). RESOLUTION, mirroring TRUSTED-PATH's own pre-USB input answer
  ("the trusted path simply stays on serial"): **on virtio-gpu-only media
  (QEMU), the trusted path stays SERIAL** (BREAK-SAK, serial episode — QEMU
  always has the UART); **framebuffer episodes + the graphical SAK land with
  simplefb-class media** (boards), where the kernel trusted sink's
  framebuffer backend + the graphical Halls dump are the same blit. On
  serial-episode media a SAK does NOT suspend tapestryd (the trusted
  conversation is on serial; the screen is not in the loop); the
  enter/leave-trusted renderer signal becomes load-bearing only where a
  framebuffer sink exists. I-27's medium-independence is preserved — the
  MEDIUM determines the episode surface, bound once at boot from the DTB
  fact, exactly as TRUSTED-PATH §7 specifies.
- **The `/dev/cons` Aurora backend is a renderer-held drain/feed fid pair**
  (the LS-8 pump pattern, inverted): `cons.c` gains a backend selector (DTB
  medium fact, TRUSTED-PATH §7's binding); on the Aurora backend, console
  output bytes ring into a drain fid Aurora reads (pollable/multishot-
  readable — the LoomSource seam Kaua already documents), and Aurora writes
  keyboard input into a feed fid that enters the EXISTING LS-8 line
  discipline (cooking, ECHO, ISIG unchanged — the discipline is
  backend-independent). The pair is granted only to the bound renderer Proc.
  Honest TCB note: the renderer sees console I/O in NORMAL operation only;
  during a framebuffer episode it is suspended (strong model), and on
  serial-episode media the trusted conversation never touches it.
- **Input ownership at stage 0**: tapestryd's warden manifest binds BOTH
  `virtio:16` (GPU) and the virtio keyboard/tablet (its allowance carries
  all its devices; `crash-probe` re-homes off `virtio:16` at G-1). The
  trusted-TIER keyboard path (MENAGERIE §7, kernel-scanned SAK combo) is a
  BOARD concern that lands with USB-HID on Lazarus — QEMU media keep the
  serial SAK per the binding above, so virtio-input routes untrusted to
  tapestryd, which is correct there.

### 18.8 T-1 formalized + the spec gate (V3)

**T-1 (no torn scanout), the binding statement**: every page of a weave stays
backed, mapped-membership-immutable, from the weave's first client map to its
retire; a present op's lifetime brackets its `TRANSFER_TO_HOST_2D` (the host
DMA-read window); a weave retires only after (a) every in-flight present on it
reached its terminal CQE (quiesce, #898) and (b) scanout composition no longer
references its resource. Reserves its ARCH §28 number at G-2/G-3 landing, per
§6.

**`specs/tapestry_present.tla`** (model-first at G-2, TLC-green before the
tapestryd impl):

- State: per-weave slot machine {FREE, DRAWN, INFLIGHT, DISPLAYED} x weave
  lifecycle {WOVEN, LIVE, REWEAVE, RETIRING} x the share/mapping refs (#847
  dual-refcount abstracted) x in-flight present multiset x the event CQ.
- Invariants: `NoTornScanout` (no INFLIGHT slot's weave loses backing/refs),
  `RecycleGate` (no draw into a non-FREE slot; CQE is the sole freeing edge),
  `ExactlyOneTerminalPerPresent` (I-29 composition), `ShareBoundedByFid`
  (clunk/death drops the client mapping; retire only via quiesce),
  `ReweaveOrdered` (old weave outlives its last in-flight present; new weave
  receives no present before its map).
- Liveness: `EventuallyRetired` (destroy/death leads to full teardown).
- Buggy cfgs (executable counterexamples): `premature_reuse` (slot freed
  before terminal CQE -> NoTornScanout), `retire_during_transfer` (teardown
  skips quiesce), `reweave_without_quiesce` (old weave freed with a present
  in flight), `map_after_retire` (a stale share claim resolves).

### 18.9 The build arc — G-0..G-9 (two phases, each row status-doc'd + gated)

| # | Phase | Chunk | Contents | Gate |
|---|---|---|---|---|
| G-0 | 9 | Agentic eyes | QMP `screendump` harness (`tools/`): verify capture under `-display none` FIRST; PNG into the agent loop; TOOLING.md ABI addendum | a captured PNG of the existing one-shot test pattern |
| G-1 | 9 | Resident GPU driver | Promote `usr/virtio-gpu` to a warden-manifested `lifecycle=persistent` driver (netd precedent); re-home `crash-probe`; render loop + IRQ-driven flush completion; fix the stale probe-only comment | boots resident; pattern persists; SMP gate |
| G-2 | 9 | Kernel delta + spec | The Weft generalization pass (naming + server-agnostic validation) + the DMA-weave `burrow_share_into` admission (§18.1) + `specs/tapestry_present.tla` model-first + focused audit (audit-bearing: joins the Weft row) | TLC-green + audit close + weft.tla cfgs re-run |
| G-3 | 9 | tapestryd stage 0 | The V1 minimal compositor: one fullscreen surface, `/dev/tapestry` `ctl`+`surface/`, present + event fids, FRAME clock, virtio-input consume; `libtapestry` folded onto `libthyla_rs::loom` (the POC seam cashed) | tapestry-demo plasma LIVE via screendump |
| G-4 | 9 | Aurora renderer MVP | Cell grid + baked font + VT-parser subset as a tapestryd client; the `/dev/cons` drain/feed backend plumbing (kernel, audit-bearing); ut on a monitor | the fbcon claim: login + `ls` via screendump; ls-gfx expect scenario |
| G-5 | 9 | Graphics audit round | The reserved rows enforced: T-1 lands in §28; tapestryd/cons-backend/DMA-share focused audit; SMP gate | clean close; v1.0-rc's optional fbcon claim satisfied |
| G-6 | 10 | The compositor | `pane/` + `layout` (§15/§18.5), split/tab/stack modes, focus + Super-chord, D5/D6 re-parenting, the full resize protocol (§18.3), multi-surface | the acceptance battery on synthetic clients |
| G-7 | 10 | SDL seam + Quake gate | `SDL_thylacine` backend (Pouch) -> libtapestry; software-Quake/Doom renders + plays | the §17 API acceptance gate |
| G-8 | 10 | Halcyon core | The transcript pane (Helix-modal), native TTF rasterizer, inline surfaces, `halcyon.rc` | §14's model live |
| G-9 | 10 | Halcyon integration | Aurora-terminals-as-panes, video player, image display, the Halcyon audit + `docs/HALCYON.md` | ROADMAP §11.2 exit criteria |

The Aurora ENVIRONMENT half (session multiplexing, status band — AURORA.md §5)
sequences after G-4 as its own arc, parallel to G-5+; the renderer half IS G-4.

**G-1 AS-BUILT (landed on `gfx-1`): the resident driver is `usr/gpud` over
`virtio-gpu-pci` (`virtio-pci:16`), NOT the virtio-mmio slot.** The first
build promoted the MMIO probe literally and measured the structural blocker:
QEMU-virt packs all six populated virtio-mmio slots into ONE 4-KiB page
(stride 0x200), userspace MMIO claims are page-granular + exclusive (I-5),
and the boot survives on temporal sequencing — the transient probes release
the page before stratumd (virtio-blk) claims it for the box's life. A second
persistent claimant starved netdev-driver AND the disk (boot-fatal). PCI
BARs are per-function (the netd/#140 move), so the resident driver coexists
with everyone; the MMIO GPU device stays wired (`gpu-mmio0`) for the
one-shot P4-L kernel-test probe, whose scanout legitimately dies at its reap
(the RW-7 quiesce — the G-0 finding). Consequence for §18.7/G-3: tapestryd's
manifest absorbs the GPU as **`virtio-pci:16`** (same identity model,
transport-shifted). crash-probe re-homed to the warden-synthetic
`restart-test` node per F15. The pattern-persists gate lives in
`tools/test.sh` (every ci-smp-gate boot runs it); as-built detail in
`docs/reference/138-gpud.md`.

**G-3 AS-BUILT (landed on `gfx-1`): tapestryd stage 0 + the R2-F3 kernel
reaper — I-40 is COMPLETE (ARCH §28).** The warden's NEW `gather = all`
manifest mode collects every matched node into ONE grant/Proc, so
tapestryd's I-34 allowance carries BOTH graphics-path PCI functions —
`virtio-pci:16` (GPU) + `virtio-pci:18` (keyboard; `virtio-keyboard-pci`
added to run-vm.sh for the same measured co-page rule, the MMIO keyboard
staying for the P4-K probe). The §18.7 "binds BOTH ... its allowance
carries all its devices" binding is realized transport-shifted (PCI, per
the G-1 rule). gpud retired (absorbed). Stage-0 realization notes: the
present engine is SYNCHRONOUS (the quiesce set is empty at every retire
by construction — the pipelined-controlq G-6 lift must land a real drain
first, the recorded SPEC-TO-CODE obligation); the Tweft mint is lazy +
idempotent per surface (`armed` at first Tweft; the Map guard is timing-
indifferent); a retired surface's event stream ends in EOF (the
queued-CLOSE record rides the pane layer, G-6); client event reads are
single-shot re-armed (the multishot + provided-buffer-pool client lift
is a G-6 seam; §18.4's multishot idiom stands as the design). The
`weave` geometry read reports `slot_stride` explicitly (a compatible
§18.5 refinement). F2 per-conn scoping rides each client's OWN
open=connect session (the fid IS the capability; deliberate session
sharing shares surfaces — the Plan 9 semantic). The per-boot pattern
gate EVOLVED: tapestry-demo presents the -v quadrant pattern + a live
plasma through the FULL protocol, and test.sh adds a liveness
double-dump (two captures must differ). As-built:
`docs/reference/139-tapestryd.md`; the server-half spec map:
`specs/SPEC-TO-CODE.md::tapestry_present.tla`.

**G-4 AS-BUILT (landed on `gfx-1`): the Aurora renderer MVP + the
`/dev/cons` drain/feed kernel backend — the fbcon claim.** The §18.7
drain/feed binding is realized as a MIRROR TAP in `cons_emit` (program
output + line-discipline echo both cross it): on serial-bearing media the
UART path continues byte-identical — the tee keeps the tooling ABI, the
host terminal, and the serial trusted path working — and the exclusive
switch (suppressing EL0 serial output, bound from the DTB medium fact per
TRUSTED-PATH §7) is the recorded board-era seam that will gate
`uart_putc`, composing with the tap. The F8/R2-F6 gate landed as
`SPAWN_PERM_CONSOLE_RENDERER` (console-attach-only grant + single-holder;
the pair open- AND I/O-re-gated in devdev), the third console role beside
ATTACH and OWNER. The feed's `is_break` is hardwired false — no SAK
forgery, by construction. Aurora is an ORDINARY tapestryd client
(libtapestry): the Cornucopia baked atlas (committed; box drawing
procedural), a VT subset (truecolor SGR; real alt-screen; DECSTBM
ignored — the MVP seam), the Bonfire palette, a blinking cursor; its
event loop BLOCKS on the Loom CQ (the non-SQPOLL pump — a poll-only loop
starves its own completions, measured). joey spawns it as the resident
boot presenter; tapestry-demo stays baked for manual runs (first-present-
wins scanout would race two residents). The per-boot gate EVOLVED again
(never dropped): `screendump -c` asserts the rendered-console signature
(exact Bonfire bg dominant + exact-fg text) + a retry-compare liveness
(cursor blink); `tools/interactive/ls-gfx.exp` is the full claim — serial
login + `ls` with before/after dumps, then QMP-typed `whoami` on the
display-bound kbd-pci0 asserted via the serial TEE (the graphical input
loop, no pixel OCR). As-built: `docs/reference/140-aurora.md`.

**G-6a AS-BUILT (landed on `gfx-2`): the pane tree + multi-surface
composition — the compositor's first stage.** `pane.rs` realizes the §14
container model (flatten-same-mode / nest-different-mode splits; split
focuses the new empty leaf = the auto-host target; collapse; root
persists; monotonic never-reused pane ids); surfaces host at CREATE
(focused-empty-leaf else split-focused). Scanout is a mode machine
(Boot/Off/Direct/Composed): the single-visible-display-sized case keeps
the stage-0 DIRECT zero-copy path byte-identical (aurora's boot
unchanged), everything else composes into a compositor-owned screen
buffer — a WEAVE-subtype DMA chunk (the §18.1 type discipline: scanout
backings are the weave class; plain SYS_DMA_CREATE is the
virtqueue class, 1 MiB-capped — measured), never registered for
sharing. EVERY switch onto a client resource rides a present-COMPLETE
(F16 uniformly, `pending_direct`). The tearing-freedom invariant:
client weave bytes are read only inside the present dispatch for the
just-presented slot; chrome repaints never touch client memory, and a
geometry-signature split keeps focus-ring moves from blanking idle
content. **The §18.3 CONFIGURE emission half is pulled forward**
(chunk-completeness): same-size CONFIGURE = the redraw request (emitted
at structural composed repaints to all visible surfaces + at
pending-direct entry), because an accumulator client's slots are
patchwork (aurora measured — static rows never healed without it); the
resize-ack/reweave half stays `E_OPNOTSUPP` until G-6b. The 9P tree
gains `layout` + `pane/<id>/{ctl,mode,role,tag,surface,geometry}` (V4
§18.5); pane files are global — the environment-role mutation gate + the
D5 layout-observability caveat are the recorded Halcyon-era seam. **This
is the intended WM-control model, not a per-surface ACL** (control is
9P / layout-as-9P; the i3-IPC / tmux-control-mode shape — a WM-control
client like `halcyon.rc` drives layout globally). Surfaces (pixels /
events / present / weave) stay F2-owner-gated; the *layout tree* is a
session-global control surface. **The G-6d weighing** (holotype F1, P2):
the sharpest consequence of a global control surface is that a session
peer could `close` another client's pane — for the console renderer
(aurora) that queues `TEV_CLOSE`, which exits it and darkens the
graphical console — or focus-steal its input. **The v1.0 trust boundary
is the per-territory `/srv` (I-1/I-28)**: `/srv/tapestry` lives in the
driver's own territory, unreachable from a user session's namespace, so
only the trusted boot chain is spawned where it can connect — no
untrusted tapestry client exists at v1.0. The per-client
layout-control CAPABILITY (a WM-control client holds it; ordinary
surface clients don't) + renderer-role protection + the D5 read ACL are
the Halcyon-era multi-untrusted-client fix (task #42); a partial
owner-scope now would break the global-WM model (the no-overfit trap).
The global `clock-rate` ctl is the same same-session-trust family;
`test-mode`/`tick`/`release`/HOLD are dev-build-only (the `test-mode`
cargo feature, stripped to `E_OPNOTSUPP` for production — the #880
class), with `release` additionally F2-owner-gated. Gate:
`ls-gfx-panes.exp` + `/bin/tapestry-battery` (structure + exact pixels
via `screendump -P`/`ppm-sample` + QMP focus legs + the collapse coda);
ls-gfx / ls-gfx-live / the per-boot `-c` gate stay green. As-built:
`docs/reference/139-tapestryd.md` "The compositor stage 1".

**G-6b AS-BUILT (landed on `gfx-2`): the resize protocol + pane close —
weave generations.** The §18.3 resize protocol is live. A surface's
`weave`/`resource_id` name its CURRENT generation (GPU resource ids are
per-generation, minted above SCREEN_RES so a fresh resource never
aliases the old — closing the #317 stale-content class); `alloc_weave`
(the shared body of `create` + `resize_ack`) allocates a full
generation, `release_gen` tears one down in the R2-F5 order. A
size-changing CONFIGURE offer records `offered = (serial, w, h)` (only
the latest is ackable — coalesce-by-replacement); the structural
composed repaint offers every visible hosted surface its pane's EXACT
content size. The ack `resize W H <serial>` on the surface ctl is THE
GENERATION FENCE — `resize_ack` mints the new generation FIRST (a
failure leaves the current one untouched, the offer standing), swaps the
surface onto it, and ONLY THEN sends the Rwrite (reply-after-alloc,
R2-F5); the conn stream is FIFO, so every post-ack present validates +
blits against the new geometry. Bounded to <=2 generations
(`old_weave.is_some()` → E_AGAIN); a stale serial → E_AGAIN, an
unknown/mismatched echo → E_INVAL — none consume the offer. The
displaced generation drains PASSIVELY (its last content stays displayed,
never read again — tearing-freedom holds) and retires at the first
post-fence present (the spec's `RetireDisplaced` + `ServerRelease`: the
display shows g2 content, so quiesce holds by construction). The client
half (`libtapestry::Surface::handle_configure` → `reweave`) opens a
fresh weave fid, re-maps, then clunks the old fid (map-new-before-
clunk-old — the client stays mapped throughout; #847 keeps g1's pages
until then). Pane close delivers a queued `TEV_CLOSE` exit REQUEST
(distinct from a retired surface's stream-end EOF) — a compositor close
strands the surface but never force-retires it (the client may need to
save). `weft.tla`/`tapestry_present.tla` UNCHANGED (the reweave is
already modeled with `ALLOW_REWEAVE`; retire-on-first-post-fence-present
is the scheduling refinement under its permissiveness — the
SPEC-TO-CODE reweave map). Aurora (the fbcon) deliberately does NOT ack
a size change (fixed cell grid) — crop/ignore posture; a reweaving
fbcon is a follow-up. Recorded seam: `weave_va_next` is a monotonic bump
(no free-list; bounded — a display weave is ~12 MiB, the 47-bit VA holds
millions of reweaves — a v1.x free-list closes it). Gate:
`ls-gfx-panes.exp` grows the resize legs (the ack negative probes + the
exact-fit reweave) + the TEV_CLOSE leg. As-built: `139-tapestryd.md`
"The resize protocol + pane close (G-6b)".

**G-6c AS-BUILT (landed on `gfx-2`): chords, focus events, tab/stack
visuals, move/zoom, multi-rect, determinism mode — the compositor's
interaction layer.** (a) **The Super chord layer** (§14 layer 1) is
intercepted in the input drain, ABOVE the event stream (§18.4): while
Super is held every non-modifier key is compositor input — bound chords
act, unbound ones drop, none reaches a surface (the WHOLE plane is
reserved, so no client can come to depend on a Super combo); a
swallowed press swallows its release/repeats even if Super lifted
first, and a key pressed before Super keeps flowing. The baked
i3-flavored table (compositor policy, a halcyon.rc concern eventually,
like the keymap): Super+arrows = spatial focus, +Shift+arrows = move,
h/v = split, f = zoom toggle, t/s = tabbed/stacked, e = split-toggle,
Tab/+Shift = tab cycle, Shift+q = close the focused pane. (b)
**TEV_FOCUS** (kind 7, value 1 gained / 0 lost, the F5 never-drop
class) emits from the single reconcile tail on every focused-surface
change. (c) **Tab/stack indicator strips** (D7 glyph-free — colored
segments, never text): a tabbed container carves TAB_STRIP_H=5 rows
into one segment row (1px gaps), a stacked one carves a row per child;
the active child's segment lights FOCUS_COLOR on the focus path,
ACTIVE_COLOR off it, BORDER_COLOR inactive. Strips are chrome
(repainted with borders on focus-only epochs; never client memory —
tearing-freedom intact). (d) **Move** (D6): directional re-parenting —
swap with the matching-axis sibling, pull out of a nested subtree
beside it (dissolution-safe index bookkeeping), wrap the root on a
pure cross-axis move; at the screen edge it is a no-op. Tabbed matches
the h axis, Stacked the v axis (moving walks tab order). (e) **Zoom**
(§14 pane-zoom, tmux-shaped): a by-id toggle; the leaf alone fills the
display (the tree untouched; a display-sized surface goes DIRECT
zero-copy); structural mutations and focus-elsewhere auto-unzoom.
(f) **Multi-rect presents**: rect_count k >= 2 rides rects 1..k INLINE
after the 32-byte header (payload 32 + 16*(k-1), count capped at 64,
EVERY rect validated before any pixel work). This REALIZES D4's
"compositor case" and supersedes the §18.2 provisional
`buf_idx_or_off` slice sketch: under D3 the present payload already
lives in the client's registered buffer, so a separate slice reference
would be redundant indirection — the inline array preserves the
registered-buffer intent with zero extra machinery. (g) **Determinism
mode** (§18.6 + F13 + F15) behind the `test-mode` cargo feature
(default-on for the dev tree; production builds strip it —
`--no-default-features` — and every verb answers E_OPNOTSUPP, the #880
class): `test-mode on` freezes the FRAME clock (the serve loop stops
wall-clock ticks — queued FRAME events drain normally, which IS the
F15 transition discipline for a synchronous single-threaded engine;
the anchor re-seats on unfreeze so no backlog fires), `tick` drives
time one step per write, and `TPRESENT_HOLD` presents run their pixel
work normally INSIDE the dispatch (transfer/blit — tearing-freedom
holds for held presents too) but defer the device-visible push
(flush / screen transfer+flush) until `release [<surface>]` (F13;
ownership-gated per F2; holds union, most-recent bytes win; a
non-HOLD present or `test-mode off` flushes implicitly; a hold cannot
complete a pending scanout SWITCH — E_AGAIN — and a hold staled by a
scanout-mode change drops, superseded by the structural repaint).
Layout grammar grows `move <id> <dir>`, `zoom <id>`, and the id-less
`focusdir <dir>` / `tab next|prev`. Gate: the battery grows the
focus-event / multirect / tabbed-strip / tab-cycle / zoom / move /
chord / test-mode / hold legs with per-stage pixel dumps
(`qmp-sendtext.sh -k` injects the Super chord). Kernel byte-unchanged;
the spec suite is unperturbed (presents model identically; HOLD defers
only sub-CQE flush granularity, below the model's abstraction).
As-built: `139-tapestryd.md` "The interaction layer (G-6c)".

### 18.10 Audit-trigger + scripture sync obligations

At each landing, per standing discipline: G-2 joins the Weft/burrow rows
(CLAUDE.md + ARCH §25.4) with the DMA-admission prosecution; G-3/G-4 add
tapestryd + cons-backend rows; T-1 takes its §28 number at G-2/G-3; ARCH §17 +
NOVEL #4 + ROADMAP Phase 9/10 sync to this section in the same scripture
commit that lands it; the per-chunk reference docs
(`docs/reference/1xx-tapestry*.md`) begin at G-1.

### 18.11 Round-1 holotype close (amendments, 2026-07-17)

The §18 design pass was prosecuted by the Fable-max holotype reviewer + a
concurrent self-audit (0 P0 / 4 P1 / 6 P2 / 6 P3; the self-audit independently
found 8 of the 16, the reviewer added the DMA-I-5 and Weft-lifecycle P1s). No
P0. The four already-voted decisions (V1-V4) stand; the findings are gaps and
unsound *bindings*, closed here as binding amendments. Closed list:
`memory/audit_tapestry_design_closed_list.md`. The load-bearing corrections
(F1, F3, F6, F16) are also folded INLINE above (§18.0/§18.1/§18.3); this
subsection is the authoritative resolution record and binds the newly-specified
items.

**F1 [P1] — the DMA admission stays STRUCTURAL, not behavioral.** Corrected
inline in §18.1: the share path admits only a *device-passive* DMA weave
subtype (pixels only, no device-interpreted structures), so a device-command
DMA ring is as structurally unshareable as MMIO. The KObj_DMA second-refcount
domain + the virtio-gpu-resource detach fold into the G-2 T-1 spec/audit (NOT
asserted type-independent). The client PTE attribute (Normal-WB, verified) was
already sound; F1 is about region *contents*, not attrs.

**F2 [P1] — `/dev/tapestry` is per-session isolated** (the enforcement V2 rests
on). A client sees + resolves ONLY the surfaces it created: the surface `<id>`
qids are per-session-scoped, netd-`/net`-style (the `CONN_FLAG|…|N`
per-connection encoding — a walk from session B cannot resolve session A's
`surface/<A-id>/{weave,present,event}`). `surface/new` mints an id in the
caller's session only. Absent this, B walks A's `weave` fid → the generalized
map → `burrow_share_into` maps A's framebuffer into B (cross-client
screen-scrape + deface) — so this is scripture, not an impl detail, and lands
with G-3.

**F3 [P1] — the arm/re-arm/disarm weave lifecycle needs real machinery**
(corrected inline in §18.0/§18.3). (a) Re-map uses a FRESH weave fid per
generation (§18.3) — `SYS_WEFT_MAP` is idempotent-per-fid (`priv->weft` cached,
cleared only at `dev9p_close`), so "re-map the same fid" would return the old
VA. (b) Disarm: retire/reweave runs a state-aware `weft_share_unregister(share_id)`
atomically with the RETIRING transition — otherwise a retired weave's `share_id`
lingers claimable in the registry until tapestryd dies, and a `Tweft` racing the
retire maps a retiring weave (the spec's `NoStaleMap` violation, whose clean
guard `Map`'s `wstate ∈ {woven,live}` has no impl counterpart without the
unregister). Both scope into the G-2 Weft pass; the G-2 SPEC-TO-CODE map MUST
bind `Map`'s guard to the concrete atomic claim-vs-retire gate.

**F4 [P1] — the compositor crash/restart contract** (a persistent warden-driver
crashes + restarts; MENAGERIE §4). On a tapestryd crash: `weft_share_release_owner`
frees the registry slots; the client's `burrow_share_into` mapping keeps the
*pages* alive (#847 mapping_count — no UAF), but the KObj_DMA + the virtio-gpu
2D resource die with the old tapestryd, so scanout is semantically dead. The
client-visible contract: the weave/present/event fids return a distinguished
**"compositor gone"** error (the #841/#845 session-dead path); the client
re-attaches `/dev/tapestry` and re-creates its surfaces (the netd Plan-9
reconnect shape — MENAGERIE §4). The spec gains a `ServerDeath` action driving
every generation to RETIRING with the client mapping still live, proving #847
holds (pages alive, no UAF) while the resource is gone (§18.8; landed with this
amendment).

**F5 [P2] — the event never-drop set + bounded buffer.** CONFIGURE, CLOSE,
FOCUS, and key press/release transitions are NEVER dropped; only FRAME and
pointer-motion coalesce. tapestryd holds a BOUNDED per-surface event buffer; on
overflow it drops-oldest among the coalescible class only (never a control
event). Present-CQEs (the D1 recycle gate, pacing-critical) get reserved CQ
headroom — or present and event ride SEPARATE rings — so an event burst never
stalls the recycle gate (the Loom-5 CQ-full HOLD must not back-pressure
presents). A dropped CONFIGURE would deadlock §18.3 (the client learns sizes
only there), so CONFIGURE's never-drop is load-bearing.

**F6 [P2] — ≤2 live weave generations** (corrected inline in §18.3 step 4): at
most one reweave in flight per surface; a resize burst queues rather than
stacking g3-while-g2-drains. This is the impl rule that makes the spec's
2-generation model (`Gens = {g1,g2}`) faithful rather than accidentally small;
the spec header now states it.

**F7 [P2] — the graphical login is NOT trusted on virtio-gpu-only media.** On
QEMU/virtio-gpu media the trusted path stays serial (§18.7), so an Aurora-rendered
graphical login/elevation prompt is a *convenience surface, not a trusted
episode* — its passphrase transits UNTRUSTED tapestryd + the virtio keyboard.
corvus authentication (login, imperium, first-credential mint) occurs on the
serial trusted path; the graphical prompt must never be presented as trusted.
(Mirrors TRUSTED-PATH §8's honest serial-asymmetry statement; confined to
dev/recovery by TRUSTED-PATH §11, which covers QEMU. The framebuffer trusted
episode + the graphical SAK arrive with simplefb-class board media.)

**F8 [P2] — the `/dev/cons` drain/feed grant gate is named.** The pair is handed
to exactly the Proc the warden binds as the framebuffer renderer, via a spawn
perm analogous to `SPAWN_PERM_CONSOLE_OWNER` (single-holder; a second opener is
refused under `g_proc_table_lock`). Without a named gate, any Proc opening the
drain fid reads all console output (leak) and injects via the feed fid (into
cooked/ECHO/ISIG) — the F2 isolation class applied to the console backend.

**F9 [P2] — per-client resource caps** (the I-32/#65 composition for the
compositor). tapestryd enforces a per-client surface-count cap + a
weave-dimension cap at `create`, rejecting past a floor — the weave is
tapestryd's DMA allocation (charged to tapestryd, drawn from the finite global
DMA pool), so the client's own #65 page cap does not bound it. `WEFT_MAX_SHARES`
bounds only the transient registry, not live weaves. The DMA-pool aggregate is
recorded as the compositor's I-32/#65 composition point.

**F10 [P2] — the framebuffer map path is its own branch** (not a reuse of the
netd Tweftio path). `weft_binding_alloc` rejects `pages == NULL` (a DMA weave)
and builds a Tweftio ring view irrelevant to a passive framebuffer. The
framebuffer-weave map claims (`weft_share_claim`) + `burrow_share_into`s WITHOUT
the Tweftio binding/ring-view, and reports stride/offsets from the KObj_DMA
geometry (not `burrow->pages`). Scoped into G-2; part of the "real machinery"
F3 corrects.

**F11 [P3] — the CONFIGURE serial is full-width.** The full 32-bit serial rides
the tevent `value` field for CONFIGURE; the client reads the new W/H from
`geometry` (a `read`) rather than packing dims into `value`. (Removes the
16-bit-truncation ambiguity; F6's ≤1-outstanding-configure bound would also
make mod-2^16 safe, but full-width is cleaner.)

**F12 [P3] — non-I/O-coherent-backend coherency** (stated now, deferred to
Lazarus). The client draws Normal-WB into the device-passive DMA weave; on an
I/O-coherent backend (QEMU/virtio) the device sees the writes. On a
non-I/O-coherent board backend a cache-clean-to-PoC before the present's
`TRANSFER` is required — define the mechanism (a present-time flag, or a
coherent-only weave allocation) with the Lazarus backend. The entire G-0..G-5
QEMU arc is unaffected.

**F13 [P3] — `TPRESENT_HOLD` release** is a `ctl` verb `release [<surface>]`;
multi-hold semantics = release composes the most-recent held present per
surface (§18.6).

**F14 [P3] — the G-0 screendump fallback.** If QMP `screendump` under
`-display none` fails on a given QEMU/backend, G-0 re-routes to
`-display egl-headless` (or `-vnc` + a framebuffer snapshot), or pulls the §16
in-band per-pane snapshot forward — a capability miss is a re-route, not an arc
stop.

**F15 [P3] — G-arc edges.** crash-probe re-homes to a synthetic restart-test
device slot needing no real device (not a real driver's slot); the virtio-input
handoff (boot-log consumer → tapestryd's allowance) is an explicit G-1/G-3
sub-step; **Loom-5 + Loom-6 complete is an explicit precondition edge before
G-3** (§18.2/§18.4 need multishot + registered buffers); a test-mode transition
drains in-flight FRAME ticks before freezing (§18.6, for golden-image
determinism).

**F16 [P3] — spec fidelity** (corrected inline in §18.3 + the spec header): the
scanout switch is at first-present-COMPLETE (aligning §18.3 prose with the
spec's `Complete`); the model pins T-1 as a *lifetime* invariant
(`NoTornScanout`/backing) with content-tearing pinned separately by
`RecycleGate` (a header line now says so); the G-2 SPEC-TO-CODE map binds
`Map`'s `wstate` guard to the concrete claim-vs-retire gate (per F3).

### 18.12 Round-2 holotype close (the fixes re-audited, 2026-07-17)

The round-1 amendment (§18.11) was itself prosecuted by a second Fable-max
holotype round (the dirty-close discipline — invasive fixes breed fresh gaps) +
a concurrent self-audit: **0 P0 / 0 P1 / 4 P2 / 2 P3**, all on the two most
invasive fixes (F1's DMA narrowing, F4's crash contract + spec action) + F5's
buffer policy. No unsoundness; the spec is internally consistent. Closed here.
Closed list: `memory/audit_tapestry_design_closed_list.md` (round 2).

**R2-F1 [P2] — the device-passive DMA claim was still behavioral + is an ABI
add.** Corrected inline in §18.1: the guarantee is structural ONLY if the kernel
MINTS + BINDS a weave-backing Burrow type (handed only to `RESOURCE_ATTACH_BACKING`,
never a virtqueue), not via a creator-asserted flag; and `t_dma_create` is
two-arg today, so this is an **ABI addition requiring user signoff at G-2**. The
honest fallback until then: the guarantee rests on tapestryd's existing
sole-GPU-owner trust (not MMIO-parity structural). The reviewer's ground truth
(`SYS_DMA_CREATE` two-arg; `burrow.c` DMA-rejected-wholesale + BURROW_TYPE
create-immutable) is correct.

**R2-F2 [P2] — the round-1 `ServerDeath` spec action was empirically vacuous
(2840 == 2840).** FIXED in the spec: the `#847` dual refcount is now modeled
EXPLICITLY (`serverRef` = handle_count, distinct from `mapped` = mapping_count
and from `backed`), a graceful `ServerRelease` drops the server ref only after
quiesce, `ServerDeath` drops it at once (even with a transfer in flight), and
the new `RefImpliesBacked` invariant (either ref ⇒ backed) is the
`#847`-across-crash no-UAF check. TLC re-run: clean GREEN at **5413 distinct
states (up from 2840 — the state jump IS the non-vacuity proof)**; liveness
GREEN incl. `EventuallyRetired` across the crash; all four buggy cfgs still fire
their exact invariant. The claim is now a real machine-check, not framing.

**R2-F3 [P2] — the crash contract designed-in an uncharged pinned-DMA leak.**
Bound: `burrow_share_into` bumps `mapping_count` only (never the client's #65
`page_count` — F9 confirms), so post-crash a client mapping pins tapestryd's
freed DMA pages charged to nothing live. Resolution: the crash contract adds a
**kernel force-reclaim of orphaned weave mappings after a bounded grace period**
(a client that ignores "compositor gone" and never re-attaches takes a
`snare:segv` on the stale mapping — it was warned); AND the share is charged to
a per-client shared-mapping budget (the I-32/#65 composition axis F9 opens), so
the pin is bounded + accounted, not an unqualified "#847 keeps pages alive"
feature. Both land with the G-2/G-3 crash-contract impl.

**R2-F4 [P2] — the never-drop set + bounded buffer had no overflow disposition
(a stalled client → compositor DoS).** Bound: the droppable class is exactly
{FRAME, PTR_MOVE}; keys/PTR_BTN/SCROLL/CONFIGURE/CLOSE/FOCUS are non-droppable
and a stalled client fills the bounded buffer with them. Resolution: (a)
**present and event ride SEPARATE rings** (resolving the §18.11-F5 A/B — event
back-pressure then never touches the present recycle gate); (b) when the
non-coalescible class alone fills a surface's bounded event buffer, tapestryd
declares the surface **WEDGED and force-retires it** (queue/deliver CLOSE, drop
the session) rather than blocking (self-DoS) or dropping a control event — the
never-drop guarantee holds for a LIVE client and terminates a dead one.

**R2-F5 [P3] — F3's "atomic with the RETIRING transition" mislocated the
ordering + the reweave-open was unordered.** Sharpened: the real obligations are
(i) the kernel **registry-removal-before-page-free** and (ii) the kernel
**claim-join re-checks the registry** (a join that wins sets `mapped`, blocking
`Free`; a join after removal fails-closed) — this is what the G-2 SPEC-TO-CODE
binding of `Map`'s guard realizes, NOT an atomic on tapestryd's local state. And
§18.3 step 2 gains: the `resize`-ack `Rwrite` reply follows g2 allocation, so
the client's fresh-fid open is guaranteed to bind g2 (never resolve to g1).

**R2-F6 [P3] — reusing `SPAWN_PERM_CONSOLE_OWNER`'s shape risked a third console
role muddying I-27.** Sharpened: the drain/feed grant (F8) uses a DISTINCT
`SPAWN_PERM_CONSOLE_RENDERER` (name provisional), stated orthogonal to BOTH
console-ATTACH (no elevation authority — already implied by the §18.7
suspension) AND console-OWNER (the drain/feed conveys no Ctrl-C-target
authority), preserving the round-1-verified I-27 ATTACH-vs-OWNER split as a
three-role clarity — the renderer is neither the elevation gate nor the
interrupt target.

**Round-2 verified-sound (do not re-litigate):** the fresh-fid + separate-map-
branch (F3/F10) compose; F2 per-session scoping composes with F3/F4 (re-attach =
a new session, no cross-session leak); the ≤2-generation bound is faithful for a
single reweave; no two §18.11/§18.12 resolutions contradict; the inline
§18.0/§18.1/§18.3 edits are consistent with their §18.11/§18.12 entries; the
spec's four buggy cfgs still violate their exact named invariant after the
dual-refcount addition.

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
