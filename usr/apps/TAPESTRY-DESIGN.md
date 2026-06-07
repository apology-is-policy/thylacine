# Tapestry -- a graphics fast-path woven on Loom (design + native POC)

**Status: design + native proof-of-concept (auxiliary track).** Authored to be
folded into the Loom arc (`docs/LOOM.md`) by the main agent. The POC under
`usr/apps/libtapestry` + `usr/apps/tapestry-demo` compiles (the auxiliary track
never boots); the kernel-side pieces are the main agent's to build.

## Naming

A **Loom** weaves threads into fabric; a **Tapestry** is the woven picture a
loom produces. The client *operates a Loom ring* (the io_uring-inverted 9P
transport, `docs/LOOM.md`) to present a **Tapestry** -- the framebuffer surface.
Loom is the instrument; the Tapestry is what the client weaves onto the display.
The display server that owns the GPU and scans out tapestries is **`tapestryd`**
(working name).

## Thesis: graphics needs ZERO new Loom core

LOOM.md's central win over io_uring is "no new opcode namespace -- 9P is the
vocabulary, uniform across files / `/net` / `/proc` / `/srv` / devices." A
framebuffer present is that uniformity cashed in once more:

- The **framebuffer** is a shared Burrow the client draws into directly
  (zero-copy). It is NOT a Loom payload buffer -- the host DMA-reads it during
  the server's `TRANSFER_TO_HOST_2D`, not over the ring.
- A **present** is `LOOM_OP_WRITE` of a small rect descriptor to a graphics-
  server fid. No new opcode (decision D3).
- **Input + vsync** are a **multishot `LOOM_OP_READ`** (Loom-5) on an event-fid:
  arm once, a CQE per event forever.

So **every multimedia fast-path -- present, input, audio -- is the same shape:
a 9P server + Loom-5 multishot + Loom-6 registered buffers, with zero media-
specific kernel code, safe across the untrusted-app -> trusted-server boundary
by I-29/I-30.** That generality is the crown-jewel property: neither io_uring
(no capability model) nor a raw shared-memory ring (no safety) can claim it.
Graphics is the proof that Loom's generality + safety were worth their cost.

**Perf:** the present's guest-side cost is an MMIO virtqueue kick + the
completion IRQ round-trip (irq-bench p99 < 5 us). Loom **hides that IRQ behind
the next frame's CPU rendering** via the async ring -- present is fire-and-
forget + reap-later instead of a per-frame stall. The one `tapestryd` context
switch per present (~1-5 us) is <0.1% of a 4-17 ms frame and BUYS the
trust-boundary safety (a kernel-mediated I-30 pin stops an untrusted game from
TOCTOU-ing the rects or pointing scanout at arbitrary memory).

## Architecture (native below, Pouch above)

```
  Pouch:   SDL  ->  SDL_thylacine video/input/audio backend  ->  libtapestry
  Native:  libtapestry (client weave; this POC) ==== Loom ring ====+
           tapestryd (owns virtio-gpu via CAP_HW_CREATE; stratumd pattern):
              present-fid (Twrite rect) . event-fid (multishot read) . scanout
              ATTACH_BACKING the shared framebuffer Burrow -> virtio-gpu 2D resource
```

`tapestryd` owning the GPU is the stratumd-as-driver precedent (16b); it keeps
the kernel out of the GPU (microkernel posture).

## Resolved decisions (agreed with the user)

- **D1 -- present CQE = "transfer-to-host done, buffer reusable"** (the recycle
  gate). Vsync is a SEPARATE multishot event for pacing. Decoupling lets a
  triple-buffered client render ahead without blocking and pace to the display
  independently.
- **D2 -- `tapestryd` creates the framebuffer Burrow** (client asks for a W*H
  surface; the server allocates the Burrow + the virtio resource and maps it
  back). GPU-resource authority stays with the owner (the A-1b precedent).
- **D3 -- present is generic `LOOM_OP_WRITE`** to a present-fid -- Loom's opcode
  set stays pure 9P. No `LOOM_OP_PRESENT`.
- **D4 -- damage = both** a single inline rect (the game case, zero extra
  buffer) AND a rect-list in a registered buffer (the compositor case).

## The present / event protocol (the Loom op mapping)

| Client call | Loom op |
|---|---|
| `Display::new_tapestry(w,h)` | control RPC to `tapestryd` (alloc Burrow + virtio 2D resource), then `SYS_LOOM_REGISTER` the present-fid + event-fid handles |
| `present(rect)` | `LOOM_OP_WRITE(present_fid, rect_descriptor)`; `user_data` = buffer id. Server does `TRANSFER_TO_HOST_2D(rect)` + `RESOURCE_FLUSH(rect)`; reply -> CQE (recycle gate) |
| `arm_events()` | multishot `LOOM_OP_READ(event_fid)` (Loom-5); a CQE per input/vsync event; the bytes land in a registered event buffer |
| reap / wait | drain the CQ; SQPOLL (Loom-4) makes submission syscall-free; `min_complete>=1` waits (death-interruptible, #811) |

Triple buffering: 3 sub-regions of the Burrow; <=3 presents in flight; the
client paces naturally on buffer availability (stale frames are dropped by
back-pressure, not by op cancellation).

## The one genuinely-new invariant (the audit focus)

**No torn scanout: a present's framebuffer pages stay backed from submit to its
terminal CQE** -- the host must never DMA-read freed pages mid-`TRANSFER`. The
#847 dual-refcount already does the heavy lifting (the server's mapping ref
keeps the pages alive independent of the client's detach); #898's
quiesce-before-free is the teardown model. This is the read-during-flight
analog of I-29's no-stale-completion -- propose it as a Tapestry-specific
invariant alongside I-29/I-30.

## What Loom-5 / Loom-6 must provide (requirements graphics imposes)

- **Loom-5 multishot**: a persistent event stream that survives many CQEs and
  CQ back-pressure (coalescing is a server policy, not Loom's). The `/srv`
  accept-loop and the event-fd are the same mechanism.
- **Loom-6 registered buffers + native API**: the ergonomic `present`/`reap`
  wrapper libtapestry's `Loom` trait targets; a registered buffer for the
  rect-list (D4) and the event payload. (The framebuffer itself is a separate
  server-shared Burrow, not a Loom regbuf, so there is no large-regbuf
  requirement from present.)

Add graphics as an explicit Loom-5/6 **benchmark workload** so the API is shaped
to fit (a present+input+vsync event loop is the canonical interactive case).

## tapestryd (server) protocol sketch

A 9P service (the `/srv` post + the stratumd-as-driver cap grant). Per session:
- a **control fid** -- `create-surface W H` -> returns a surface id + the
  framebuffer Burrow handle (D2); `destroy-surface`.
- a **present fid** per surface -- `Twrite` of a rect (or rect-list, D4) flushes
  it: `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`.
- an **event fid** -- the multishot read source; `tapestryd` reads virtio-input
  + synthesizes Vsync from flush completion and posts events.

## The SDL seam (the on-ramp for Quake / DOSBox)

A Thylacine SDL **video backend** (`SDL_thylacine`) targets libtapestry:
`CreateWindowFramebuffer` -> `new_tapestry`; `UpdateWindowFramebuffer` ->
`present`; `PumpEvents` -> the multishot stream; SDL_audio -> a future audio
server (same shape; **no virtio-sound driver exists yet** -- a prerequisite for
game audio). Software-rendered Quake / Chocolate-Doom / PrBoom land on this;
GZDoom stays gated on a GL stack (Mesa swrast via Pouch is the realistic route,
not a hand-rolled GL).

## Unblock list (what must exist before this runs)

1. **Loom-6** -- the native Loom API (`libthyla_rs::loom`) that implements the
   `Loom` seam trait. (Loom is at ~4 today.)
2. **virtio-gpu scanout** -- CREATE_2D / ATTACH_BACKING / SET_SCANOUT /
   TRANSFER_TO_HOST_2D / RESOURCE_FLUSH (the deferred half of `usr/virtio-gpu`).
3. **`tapestryd`** -- the server above (CAP_HW_CREATE; the 9P protocol).
4. **virtio-sound** (for audio) -- does not exist; scope alongside.

## The POC (what is built here)

- `usr/apps/libtapestry` -- `Display` / `Tapestry` / `Event` / `Rect` + the
  `Loom` seam trait + `MockLoom`. The full client model (triple-buffer +
  present recycle-gate + multishot event drain), compiling against the
  documented future Loom surface. Swap `MockLoom` for `libthyla_rs::loom`
  (Loom-6) with ZERO change to `Display`/`Tapestry`.
- `usr/apps/tapestry-demo` -- an animated XOR-plasma software renderer driving
  the model. Compile-only; the MockLoom terminates it. On real hardware the
  same code displays the live plasma. See `tapestry-demo/TEST-PLAN.md`.
