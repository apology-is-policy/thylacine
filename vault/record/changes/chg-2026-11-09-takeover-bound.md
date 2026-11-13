---
id: chg-2026-11-09-takeover-bound
type: chg
title: "Every wait a takeover makes is bounded from the chord (vote 80)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
  - sub-substrate-machine
established: [dec-2026-11-09-takeover-bound-from-chord]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Round 3's F1 ([[chg-2026-11-09-trusted-round3]]) as the operator decided it in vote 80 ([[dec-2026-11-09-takeover-bound-from-chord]]). The seat stamps a generation's first sight in QUIESCING, the reply to the key that made the chord, and hands that instant to the device while the takeover is not yet up (`Gpu::bound_takeover`; cleared at activation and in every other phase, so a stall under the trusted scanout or a restore is untouched). `gpu_api::stall_verdict` takes the chord's age: at `TAKEOVER_BOUND_MS` (33 s) a control stall still outstanding latches the engine dead (`settle_stall`) and a cursor stall abandons the lane (`cursor_settle`), whatever the stall's own age, and Lictor's line says the takeover's bound fired. No renewal follows either, so the kernel's 35 s limit fails the chord after it. Host tests: the verdict at and below the bound, a retirement still reviving past it, and the bound read against `SEAT_QUIESCE_LIMIT_NS` parsed out of `seat.h`. Witness: `ls-graphical-sak-chained`, on its own lever (`Request::TestArmChained`, opcode 69, armed by tapestryd on `thylacine.test-latch-chained`), whose two shots each wait twice in series: the control queue held 8 s and then the cursor lane held past every bound (abandoned at the bound before the kernel closes the seat; the lane revives 10 s later), then the cursor lane held 8 s and the control queue withheld for good (the engine dies at the bound before the close; last, since the engine stays dead).
