# TEST-PLAN: tapestry-demo (Tapestry POC)

Status: authored, NOT executed. Verified: `cargo build --release` + clippy
clean; two PT_LOAD (RX + RW), W^X-clean.

`tapestry-demo` weaves an animated XOR plasma onto a Tapestry over a Loom ring.
Today it is backed by `MockLoom` (compile-only); the SAME source runs on real
hardware once the unblock list (TAPESTRY-DESIGN.md) is met.

## What the compile already proves (no boot)
- The client model is expressible in native no_std Rust and type-checks: arm
  the multishot event stream, drain events, render into the back buffer,
  present + recycle-gate, rotate buffers.
- The `Loom` seam compiles with a swappable backend (MockLoom today;
  `libthyla_rs::loom` at Loom-6) with no change to Display/Tapestry.

## Offline behavior (MockLoom)
- Each present completes immediately + emits a Vsync; a Close is scheduled
  after `FRAMES` (120) presents.
- Expected: the loop renders 120 frames, counts 120 vsyncs, prints
  `tapestry-demo: wove 120 frames (120 vsyncs), checksum <N>`, exits 0.
- The checksum proves pixels flow through render -> back buffer -> present
  (and stops the renderer being optimized away).

## On real hardware (the main agent, once unblocked)
Backed by `libthyla_rs::loom` (Loom-6) + `tapestryd` + virtio-gpu scanout:

| # | Scenario | Expected |
|---|---|---|
| T1 | run with a display | a 320x200 animated XOR plasma, smooth | 
| T2 | press keys | Key events delivered via the multishot stream (a real app reacts) |
| T3 | measure frame time | present's IRQ round-trip is hidden behind the next frame's render (the Loom win vs a synchronous present) |
| T4 | triple-buffer check | <=3 presents in flight; the client never blocks in `present` under steady state |
| T5 | kill the client mid-present | no torn scanout; the framebuffer backing outlives the in-flight present; `tapestryd` reclaims cleanly (the new invariant) |

## Wiring notes for the executor
- This rides the Loom arc: it cannot run until Loom-6 lands the native API +
  `tapestryd` + virtio-gpu scanout exist (TAPESTRY-DESIGN.md unblock list).
- To bring it up: implement `impl tapestry::Loom for libthyla_rs::loom::Ring`
  (the seam), point `new_tapestry` at `tapestryd`, drop `MockLoom`.
