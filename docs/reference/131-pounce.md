# 131 — POUNCE: fused walk+getattr resolution [ABSORBED INTO THE VAULT]

This document was absorbed at the stalk sweep
(`chg-2026-07-31-stalk-sweep`). Its content now lives, code-verified and
current, across:

    vault/system/kernel/namespace/sub-kernel-stalk.md   (the resolver side)
    vault/system/kernel/ninep/sub-kernel-ninep-wire.md  (Twalkgetattr 140/141)
    vault/system/kernel/ninep/sub-kernel-ninep-client.md
    vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md (walk_attrs + the latch)
    vault/record/changes/chg-2026-07-07-pounce.md       (the arc, P-1..P-5)
    vault/record/audits/adt-pounce-p5.md                (the close round)

(the run gather + the LEFT-TO-RIGHT fail-ordering post-scan, the
mount-mid-run split, `..` disabling the pounce, carried attrs, the
strict BIND/partial/QUERY shape contract, the per-session
`wga_unsupported` ENOSYS latch and its static sentinel, `STALK_STAT` +
`SYS_STAT = 88`, the consumers, and the P-4 measurements including the
Phase-2 attr-cache DEFER decision).

The P-5 P3 (the latched-fallback double base X-check) is
`seam-372-latched-double-xcheck`. Design scripture is unchanged:
`docs/POUNCE-DESIGN.md`.
