---
id: view-closed-sub-kernel-ninep-attach
type: view
title: "Do-not-re-report preamble — sub-kernel-ninep-attach"
query: closed:sub-kernel-ninep-attach
---
# Do-not-re-report preamble — sub-kernel-ninep-attach

Generated from `fnd-*` notes (`meta/lint.py --render`; also emitted
on-demand by `lint.py --closed sub-kernel-ninep-attach`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
8 closed findings on [[sub-kernel-ninep-attach]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-16c-r1-f1]] [P1] Handshake runs unbounded against a hung stratumd (fixed) — Fixed: `srvconn_set_client_deadline(cn, now + SRVCONN_HANDSHAKE_DEADLINE_NS)`
- [[fnd-16c-r1-f10]] [P3] Pivot rights-gate comment wrong about RIGHT_WRITE (fixed) — Fixed: comment corrected.
- [[fnd-16c-r1-f13]] [P3] territory_pivot_root duplicates territory_chroot's body (documented) — Documented in the function header; the shared-helper refactor is v1.x
- [[fnd-16c-r1-f4]] [P2] kernel_attached set too late (handle-alloc race window) (fixed) — Fixed: the setter hoisted to immediately after the adapter's init commits
- [[fnd-16c-r1-f5]] [P2] attached_destroy_inner freed the adapter without destroying it (fixed) — Fixed: both `p9_spoor_transport_destroy` and `p9_srvconn_transport_destroy`
- [[fnd-16c-r2-f2]] [P2] install_transport-failure path leaked the adapter struct (fixed) — Fixed: explicit destroy + kfree after the unref on that path. The same
- [[fnd-16c-r2-f4]] [P3] R1-F5's close justification was inaccurate (fixed) — Fixed: the justification replaced with the real mechanism (offset-0 magic
- [[fnd-16c-r2-f5]] [P3] Dual-destroy magic distinctness unpinned (fixed) — Fixed: `_Static_assert` pair in 9p_attach.c (magics distinct; magic at
<!-- generated:end -->
