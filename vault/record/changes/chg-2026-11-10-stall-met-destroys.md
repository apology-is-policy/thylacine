---
id: chg-2026-11-10-stall-met-destroys
type: chg
title: "A destroy a stall met, or the vindication's, is owed rather than final; the r4 P3 batch and the CREATE/DESTROY witnesses"
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
Self-found while building the vote-81 round-4 follow-ups (OPEN-BUGS, P2, pre-existing): a warp ctx destroy a stall MET left Lictor's ledger retiring the id for good, and tapestryd called it Refused, condemning the slot for good; the vindication arm's refused destroys waited for a vindication that never comes. With eight warp slots, eight destroys behind another client's deep GL queue ended GL box-wide for the boot. Lictor's `CtxDestroy` now un-retires on any failure but a dead engine: a repeat of a destroy that ran is harmless, since QEMU's CTX_DESTROY has no error path and virglrenderer only drops the handle from a table (QEMU v10.0 `virgl_cmd_context_destroy`, virglrenderer 1.1.0 `virgl_context_remove`, read in the upstream sources). tapestryd owes every destroy `failure_cause` calls a stall, the vindication arm routes through `warp_ctx_destroy_id`, and `warp_destroy_owed` frees a vindicated slot's backings when it recovers. Round 4's P3s: F4 is closed seam-side (no pub is reissued while a slot that carried it is live, condemned or owes a destroy, and a slot owing its venus destroy is not reused), not by keying Lictor's record on the device context, which would let a venus chain run past a released GL chain; F5 the latch in the docs; F6 the compose probe's id under a const assert of the property that holds; F3 ABANDON reports the post-vindication submit's fate (`after-submit S<n>R<n>`, Pi mutant M9). Witnesses: Lictor opcode 70 (`TestStallShot`) and tapestryd's `warp-stall-create` / `warp-stall-destroy` aim one stall shot at the caller's own create or destroy (8 per boot); the readback leg's CREATE and DESTROY arms judge monotone counters (`owed-incurred`, `owed-ran`, `destroy-failed`), since the owed destroy can run before the client reads anything; Pi mutants M10-M12. Host-tested (lictor 84, tapestryd 124); not booted.
