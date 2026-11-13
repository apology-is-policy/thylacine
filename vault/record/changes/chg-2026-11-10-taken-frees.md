---
id: chg-2026-11-10-taken-frees
type: chg
title: "A free a chain still holds is taken as a debt and paid once none can reach it; the owed-free pass runs after the seat's step, bounded, links first"
date: 2026-11-10
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-10
---
Vote 81 audit round 6 (Opus 5.5 on Opus 5.5): 0 P0, 1 P1, 0 P2, 4 P3; every round-5 fix judged correct. F1 (P1, pre-existing, OPEN-BUGS 2026-11-10T02:40Z): Lictor refused a resource free while a poisoned chain could reach the resource, before any ledger change, and nothing asked again; tapestryd's vindication then freed the guest half and reset the context's leak charge, so the ledger entry, the attachment and the broker's pins stayed for the boot, uncharged. A client repeating poison and destroy pinned a context's backing cap per cycle and spent a ledger entry per buffer toward `MAX_OBJECTS`. Lictor now takes such a free as a debt (`Device::take_unref`, `Gpu::owe_unsent_free`): the entry retires, the free is owed unsent, and the owed-free pass sends it once no chain can reach the resource, after a `CTX_DETACH_RESOURCE` for each link the ledger still records (`Objects::links`), since `Objects::retired` refuses a linked id; QEMU v10.0's unref detaches the guest backing itself, so the refused `DetachBacking` needs no repeat. A resource on the display, a dead engine or a debt at its bound is refused as before. F2 (P3): the pass ran before the seat's step, whose first act notices the chord, and was bounded by count only; it now runs after the step on a normal seat, looks at 16 owed frees at most in turn, and stops at 8 device commands or `SUBMIT_DEADLINE_MS` spent. F3 (P3): a ring teardown at bring-up said its extent was owed when nothing was; `free_mapped_or_owe` now reports done, owed or leaked. F4 (P3): the FREE arm witnessed one path of several; the owed-free state machine is now `gpu_api::OwedFree` with `Answer` and `unref_frees`, host-tested (links first, extent only after both acknowledgements, a stall never an answer, the give-up, no answer but OK and "no such resource" read as gone), and GPU-DESIGN 4.5.15 names what each leg witnesses. F5 (P3): three doc truths. `Request::FreesOwed` now answers `Frees`, adding `retiring`; the warp ctl prints `frees-retiring`. Witness: the readback leg's TAKEN arm poisons its ctx, destroys a BO, and passes iff the free is taken (owed, incurred and retiring +1), stays unpaid while poisoned, and is paid after the vindication with the id retired; FREE now also requires `frees-retiring` back. Pi mutants M16 (no take), M17 (the repeat ignores the chains), M18 (no link detach), each TAKEN=FAIL; M13 and M15 re-anchored.
