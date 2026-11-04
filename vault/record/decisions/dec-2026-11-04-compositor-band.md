---
id: dec-2026-11-04-compositor-band
type: dec
title: "The compositor warden designates runs in the INTERACTIVE band"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-sched]
created: 2026-11-04
---
## Fork

With every CPU busy, tapestryd's serve pass took about 250 ms against VISION 4.5's 33 ms. It ran about 7 ms in such a pass and waited for the rest: it was in NORMAL behind the hogs, while Lictor, its server, was already INTERACTIVE because `pci_irq_wait` marks every caller. ARCH 8.3 names INTERACTIVE as the default for Halcyon, and no leg built that. Every existing leg has a trust gate, and the seat client's role is explicitly untrusted.

## Research

Measured on HVF, tip 8c0b21940, two sets each, by private kernel counters (work/nov03-mm1/axes6.log). With tapestryd's threads forced INTERACTIVE, passes over 80 ms went 17,17 to 0,0 on four CPU hogs, 113,116 to 0,0 on four sleeping hogs, 3,5 to 0,0 on four faulting workers; the main thread's mean run-queue wait went 534,685 us to 6.7,6.7 us and 2661,2576 us to 3.8,3.8 us. The hogs did not pay: 3941,4060 ms to 3910,3906 ms. Not measured: anything under 80 ms, the Pi, software compositing. Plan 9's dynamic classifier (boost on wake, demote on quantum) needs no gate, and ARCH assigns it to the EEVDF lift. Slowing the seat coordinator's 100 Hz poll changed the slow passes not at all.

## Options

Tie the band to the seat role; build the Plan 9 classifier now; cut handoffs per call in userspace and stay NORMAL; leave it for the EEVDF lift.

## The call

Vote 64 of 2026-11-04, the recommended option: tied to its seat role. When warden's designation takes, the Proc's threads are INTERACTIVE: the first when it takes the role, later ones from their creation. A fork of the compositor is not. Accepted knowingly: the role is untrusted and bands have no aging, so a compositor that spins starves NORMAL on the CPUs its threads occupy, at most two.

## Rationale

The realized set stays narrow and needs no ABI change, and the gate is who designates, which is a parent-gated spawn bit. Fewer handoffs is still worth doing and does not reach the budget alone. The classifier is the right general answer and is a wider change than this stall justifies now.
