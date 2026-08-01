---
id: fnd-stalk1-r1-f3
type: fnd
title: "The borrowed-start handle_get TOCTOU, amplified to N blocking hops"
round: adt-stalk1-r1
severity: P3
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-06-04-844-handle-lifetime
created: 2026-08-01
---
## Prosecution

The surface-wide lockless `handle_get` returned a raw `Spoor *` with no
ref; `SYS_OPEN` holds that borrowed `start` across up to
`STALK_MAX_DEPTH` BLOCKING dev9p walks, so a concurrent same-Proc
`t_close(start_fd)` race window widened from single-hop to N-hop.
`SPOOR_MAGIC` yields a clean extinction, not silent corruption.

## Disposition

DEFERRED at the round — correctly: a stalk-local `spoor_ref` after an
already-racy lookup could itself ref a freed Spoor; the fix belonged to
the handle-lifetime hardening pass. That pass (#844,
[[chg-2026-06-04-844-handle-lifetime]]) closed it two days later:
`sys_lookup_spoor` now TRANSFERS a real ref the handler holds across
the resolution and clunks after (the in-commit E2E caught the initial
missing clunk). The FROM_ROOT sibling window (pivot-vs-walk, #848) was
closed separately by RW-4's `territory_root_ref`
([[fnd-rw4-sa-f1]]). The stale 104-stalk caveat teaching the pre-#844
contract was retired at the vault absorption.
