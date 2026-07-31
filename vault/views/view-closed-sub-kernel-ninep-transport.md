---
id: view-closed-sub-kernel-ninep-transport
type: view
title: "Do-not-re-report preamble — sub-kernel-ninep-transport"
query: closed:sub-kernel-ninep-transport
---
# Do-not-re-report preamble — sub-kernel-ninep-transport

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-ninep-transport`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
9 closed findings on [[sub-kernel-ninep-transport]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-16c-r1-f11]] [P3] init_destroy test's manual unref double-drop hazard (documented) — Documented (the close-before-destroy discipline comment carries it);
- [[fnd-16c-r1-f2]] [P1] Steady-state ops over SrvConn run with no recv deadline (fixed) — Fixed AT THE ROUND: a defense-in-depth auto-arm in
- [[fnd-16c-r1-f3]] [P1] KOBJ_SRV read/write did not gate on kernel_attached (fixed) — Fixed: both branches refuse when `srvconn_is_kernel_attached(cn)`.
- [[fnd-16c-r1-f7]] [P2] Server-side close tears down even when kernel_attached (documented) — Defended as INTENTIONAL asymmetry: server-side close IS the legitimate
- [[fnd-16c-r1-f8]] [P3] Teardown-migration path untested (fixed) — Fixed: `kernel_attached_skips_teardown_on_handle_close` extended with a
- [[fnd-16c-r1-f9]] [P3] Dead short-write branch in srvconn_transport_send (fixed) — Fixed: simplified away. (The #349 EAGAIN evolution later gave the send
- [[fnd-16c-r2-f1]] [P1] R1's two deadline fixes interact: a stale lapsed deadline wedges post-attach ops (fixed) — Fixed at the round (auto-arm gate widened to `deadline == 0 OR now >=
- [[fnd-16c-r2-f3]] [P3] Transport header described the pre-fix deadline story (fixed) — Fixed: header rewritten. (Rewritten AGAIN at #841 when the mechanism
- [[fnd-16c-r2-f6]] [P3] Auto-arm fires even with data already in the ring (withdrawn) — Withdrawn: MOOTED by the F1R2 gate refinement at the round (and doubly
<!-- generated:end -->
