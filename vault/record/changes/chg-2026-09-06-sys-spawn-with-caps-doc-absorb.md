---
id: chg-2026-09-06-sys-spawn-with-caps-doc-absorb
type: chg
title: "absorb docs/reference/63-sys-spawn-with-caps: zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["09aa9fae"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/63-sys-spawn-with-caps.md -> ABSORBED

Absorbed the 130-line SYS_SPAWN_WITH_CAPS reference doc into a redirect stub. The
cap-subset spawn ((parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY via
rfork_with_caps, I-2 monotonic reduction) -> sub-kernel-caps (verified at the
102-legate absorption); the spawn handler -> sub-kernel-exec /
sub-kernel-syscall-dispatch. Zero fold.

105 -> 106 absorbed of 157. lint 0-fail.
