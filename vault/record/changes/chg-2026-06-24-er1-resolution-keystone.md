---
id: chg-2026-06-24-er1-resolution-keystone
type: chg
title: "ER-1: the resolution errno keystone (stalk_err → -T_E_NOENT)"
date: 2026-06-24
arc: arc-go-build
commits: ["8f630de6"]
touched:
  - sub-kernel-stalk
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The errno-rollout's first keystone, driven by the on-device Go build:
`stalk()` returned a bare NULL and `SYS_OPEN` a bare `-1` — which Go's
Linux-shaped decode renders EPERM — so `os.IsNotExist` was never true
and every create-or-open fallback + cache existence check misfired.
`stalk_err(..., int *errp)` writes the cause as a POSITIVE `T_E_*`
(NOENT walk-miss / ACCES denial / INVAL structural / propagated /
IO default) and the handler returns `-*errp`. The `err_code` mapping
deliberately collapses the generic `-1` to `T_E_IO` — `-1 == -T_E_PERM`
and errno 1 must never surface from a sentinel. `stalk.err_codes` pins
the table. ER-2 (the per-walk dev9p errno out-param) remains in flight
on the vivarium branch ([[seam-posix-pathname-form-gates]]).
