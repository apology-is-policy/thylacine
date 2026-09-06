# 109 — devdev: the /dev char-device directory + the I-27 gate-at-namespace-open [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-devdev-absorb`). The
`/dev` aggregating directory and its trusted-path gate. Its content spans several
code-owners:

- the **`/dev` front door itself** — the aggregating directory Dev, the
  two-tier I-27 gate (open-mint on the console leaves + the per-I/O re-gate on
  the data leaf, and the **revoke-asymmetry** it produces: a devcons fd survives
  a SAK revoke while a `/dev/cons` fd dies on de-attach), the winsize leaf, the
  renderer-minted-consctl WINSIZE-verb-only restriction, the reuse-nc walk, and
  the unforgeable `spoor_is_console` / `SYS_FD_DEVCLASS` identity:

      vault/system/kernel/console-gfx/sub-kernel-devdev.md

- the **shared console implementation** behind both doors — `cons_input_read` /
  `cons_output_write`, the single-reader busy-guard bounding the console to one
  reader across both front doors, and the SAK recognizer:

      vault/system/kernel/console-gfx/sub-kernel-cons.md

- the **kernel-side boot mount** — `joey_mount_static_dev`, the kproc that
  grafts `/dev` (and `/srv`, `/proc`, `/ctl`, `/hw`, `/env`) onto the devramfs
  synth stubs, inherited by every descendant:

      vault/system/kernel/boot/sub-kernel-joey.md

- the **post-pivot re-graft and the `/dev/pts` graft** — the userspace init that
  carries `/dev` across the pivot and mounts the ptyfs tree over the `pts` stub:

      vault/system/stratum/sub-stratum-boot.md

- the **mount-table sizing** (`PGRP_MAX_MOUNTS`) and the pivot-orphan-mounts
  seam:

      vault/system/kernel/namespace/sub-kernel-territory.md
      vault/seams/seam-80-pivot-orphan-mounts.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- Its `PGRP_MAX_MOUNTS` "8 → 12 / task #80" figures are stale — the constant has
  since grown (to 32); the concept (the pivot leaves orphan mounts that a
  pivot-time GC would reclaim) is owned by the territory dossier + `seam-80`.
- It says the standalone `devrandom` Dev "is no longer reachable by any path" —
  imprecise: `devrandom` is still registered and its `init` still does the boot
  seed; only its *read path* is superseded by the `/dev/random` devdev leaf over
  the CSPRNG.
- Its revoke-asymmetry note (the #57b audit F2) was, at absorption, the only
  account of a real security property that lived in none of the three candidate
  dossiers; it is now folded into `sub-kernel-devdev` with an `inv-i27`
  precision clause (the "every door gates identically" clause is about the mint
  gate; the post-mint divergence is a tightening on the namespace path).
