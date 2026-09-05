# 56 — SYS_MOUNT / SYS_UNMOUNT [ABSORBED INTO THE VAULT]

This document was absorbed at the territory sweep
(`chg-2026-08-01-territory-sweep`). Its content now lives, code-verified
and current, in the dossier:

    vault/system/kernel/namespace/sub-kernel-territory.md

(the syscall ABI, the `STALK_MOUNT` mount-point resolution and why the
no-cross carve-out is what makes MREPL work, the `RIGHT_READ`-only
rights gate, and the authority model — namespace-mediated, not
capability-gated).

**What this file got WRONG by the time it was absorbed.** Its ABI
section HAD been updated at stalk-2 (the path-keyed form, the
`STALK_MOUNT` resolution) — but only that section, and the rest of the
file was left at P5-mount-syscall:

- `PGRP_MAX_MOUNTS = 8` in Performance — is 20.
- "9 tests" — the mount-table battery is 13.
- Path IDs described as the live keying ("userspace agreeing on
  `42 = /stratum/data` is convention") — superseded by the same
  stalk-2 change the ABI block above it documents.
- Worst: caveat 3 taught that "walking through a mount point still uses
  the Plan 9 bind table (already implemented)". The walk has NEVER
  consulted the bind table; `stalk` crosses via `mount_lookup`, and the
  bind table is unreachable dead scaffolding (no `SYS_BIND`, no
  production caller, never read by the resolver).

A partially-updated document is worse than a wholly stale one: the
current ABI block lends authority to the stale sections beneath it.

The one thing the file never stated, now recorded explicitly, is the
consequence of the gate set: **there is no write-permission check on the
directory mounted over, and no capability guards the syscall at all**.
That is Plan 9-correct — your namespace is yours, so mounting over a
directory you can merely search changes only your own view — and it is
the container keystone.

The `spoor_clunk` cclose fix this chunk also carried, and the lesson of
why it was bundled rather than split, are recorded at
`vault/record/changes/chg-2026-05-14-p5-mount-syscall.md`.

Design scripture is unchanged: `docs/ARCHITECTURE.md` §9.6.
