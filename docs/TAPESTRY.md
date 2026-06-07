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
