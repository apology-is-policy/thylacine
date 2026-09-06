---
id: chg-2026-09-06-entry-cluster-gap-folds
type: chg
title: "entry/trivial-devices cluster: fold the 2 Explore-flagged non-orphan gaps -- the #57b devcons/devcons revoke-asymmetry into sub-kernel-devdev (+ an inv-i27 precision clause) and the RNDR FEAT_RNG detection/NZCV-capture mechanism into sub-kernel-content, ahead of the 109/31 stubs"
date: 2026-09-06
arc: arc-vault
commits: ["6202dc8d"]
touched: [sub-kernel-devdev, inv-i27, sub-kernel-content]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
A read-only Explore mapped the 01-boot/31-trivial-devs/109-devdev cluster and
flagged two content gaps in dossiers that DO own the code (so they must be
folded before the docs stub, or the content drops to git history). Both VERIFIED
against the code before folding.

- **The revoke-asymmetry (109-devdev's headline gap) -> sub-kernel-devdev + a
  precision clause on inv-i27.** The two console doors gate identically at the
  MINT (console-attach at open) but diverge post-mint: `SYS_CONSOLE_OPEN`
  (devcons) gates only at open, so an already-open fd SURVIVES a SAK revoke --
  verified `cons_input_read` (cons.c:1687) re-reads attachment only to set a
  scheduling band (1711), never to authorize -- which is what lets the boot
  authority hand an attached fd down as session stdio. `/dev/cons` re-gates every
  I/O, so its fd DIES on de-attach (the stricter, fd-outlives-revoke-closing
  semantic). inv-i27's "every door gates identically" was correct about the mint
  but silent on the divergence; added a clause clarifying mint-identical vs
  post-mint-tightening (NOT a weakening -- both doors are still mint-gated). The
  POSIX-surprise caveat folded into the dossier.
- **The RNDR detection/capture mechanism (31-trivial-devs) -> sub-kernel-content.**
  The dossier abstracted RNDR to "the CPU's own generator"; folded the concrete,
  live mechanism: FEAT_RNG probe from ID_AA64ISAR0_EL1 bits[63:60]
  (g_rndr_available, random.c:82/146-149), the NZCV-capture idiom (RNDR sets
  PSTATE.NZCV, cset on ne, RNDR_RETRY_MAX=10 transient-dry retry, random.c:156-167),
  and the load-bearing "cc" clobber (RNDR writes the flags -- omitting it
  miscompiles a stale condition). Not dead code -- RNDR is one of the three seed
  inputs on capable targets.

Effort max (I-27 trusted path + CSPRNG entropy detection are both crypto/security
surfaces). No code touched; no audit owed. sub-kernel-devdev + sub-kernel-content
already at 2026-09-06; inv-i27 updated 2026-08-02 -> 2026-09-06. The 01-boot /
31-trivial-devs / 109-devdev stubs follow; kernel/joey.c remains the 2nd orphan
(blocks 109's kernel-boot-mount atom).
