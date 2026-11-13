---
id: chg-2026-11-10-owed-frees
type: chg
title: "A free the device has not acknowledged is owed and repeated; replies carry the stall count their request began at; the stall shot is bound to its connection"
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
Vote 81 audit round 5 (Opus 5.5 on Opus 5.5): 0 P0, 0 P1, 3 P2, 3 P3. F1 (the Pi specs for M10-M12 omitted `CLEAN=FAIL`) was a spec-list fix. F3 (P2, pre-existing, OPEN-BUGS 2026-11-10T01:44Z): a `RESOURCE_UNREF` a stall met, or one refused before staging or by the device, left Lictor's ledger entry retiring for good with its pins held, since `resource_unref_raw` journals only an acknowledged free and nothing repeated one; the ledger holds 4096 entries box-wide. Lictor now owes such a free (`Gpu::unref_or_owe`; a mapped HOST3D blob's unmap with it, `free_mapped_or_owe`) and repeats it at the top of each NORMAL serve pass while the engine takes commands, eight at most, each once no chain can reach it, never inside a request. An unref answered "no such resource" reads as gone: QEMU v10.0 `virgl_cmd_resource_unref` answers so for an id not in its list, and refuses an existing resource's free only when the host unmap fails, which clears the mapping, so the next repeat runs; three answered refusals give a free up, its id, pins and extent kept. `drain_condemned` and both mint unwinds route through the same owing, a create whose unwind left a free owed retires its id when the free is paid, and only frees made while serving are owed. `Request::FreesOwed` (opcode 71) reports the debts; the warp ctl prints them. F2 (P2): tapestryd judged a failed warp ctx create against the stall count of the previous reply, so a stall Lictor met between two requests had tapestryd owe a destroy for an id Lictor had released, and the owed destroy's refusal condemned the slot for good; every reply's `Stats` now carries `stalls_begun`, the count as the serve loop began the request, and the creates and `warp_ctx_destroy_id` read their failure on it. F4 (P3): four doc truths, and an answered refusal of an owed venus destroy now condemns the slot as the clean and vindication arms do. F5 (P3): the dead `let stalls` in `CtxDestroy` is gone (Pi mutant M11 re-anchored). F6 (P3): `TestStallShot` is bound to its connection's next request and spent at that request's end; the venus-create aim, unreachable, is dropped, and the venus band's owed paths stay unwitnessed by a leg (the readback host runs virgl without venus). Witness: `warp-stall-free` aims a shot at the free of the caller's next destroyed BO; the readback leg's FREE arm judges `frees-incurred`, `frees-paid` and the gauge; Pi mutants M13-M15. `TEST_STALL_SHOTS` 8 -> 12 for three arms.
