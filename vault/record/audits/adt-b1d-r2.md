---
id: adt-b1d-r2
type: adt
title: "B-1d round 2: the initrd's bin/ -- four breakers the move made, and a deny leg that stopped testing its escape"
date: 2026-09-25
scope: [sub-kernel-joey, sub-kernel-content, sub-stratum-boot, sub-substrate-build, sub-warden, sub-libhalcyon, sub-utopia-eval, sub-coreutils-filters, sub-pouch-mem]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: dirty
counts: {p0: 4, p1: 0, p2: 1, p3: 5}
findings: [fnd-b1d-r2-f1, fnd-b1d-r2-d5]
round-of: chg-2026-09-25-b1d-round2-close
prior-round: adt-b1d-r1
created: 2026-09-25
---
## Scope

WIP 7 (54892232), the initrd's `bin/` layout
([[dec-2026-09-25-initrd-bin-directory]]), and the WIP 8 deltas sent to the
round while it ran: the kernel's `bin/joey` lookup and kproc's dot stamped at
`/bin`, joey's chdir to `/bin` and back to `/` at the pivot, the `/bin` bind's
source, build.sh's staging, the probes' path moves, the shell's `$path` and
libhalcyon's `PROG_DIRS` dropping `/`, the warden's driver path. Opus 5.5 at
max, the fallback tier (Fable 429).

## Convergence

Dirty. Four P0-class breakers closed during the round, each also found by the
build, a boot or the self-audit: build.sh:409 defined `ramfs_bin` from itself
(the bulk rename rewrote its own definition); `devproc.read_cwd` expected
kproc's dot at `/`, which `kernel/main.c:854` stamps at `/bin` before the
suite; coreutil-smoke's `realpath rel` expected `/y`; and joey handed
`bin/system.key` to the single-component `t_walk_open`
([[fnd-b1d-r2-d5]]), which the reviewer reported before the boot reached it.
Open after them: 0 P0 / 0 P1 / 1 P2 / 5 P3. F1 ([[fnd-b1d-r2-f1]]): the dlopen
prover's `../` escape stopped testing an escape when the working directory
moved to `/bin`. F2 (nothing outside the kernel tests kept the initrd root to
`bin/` and `lib/`), F3 (joey's post-pivot chdir had no witness) and F4 (stale
prose) were fixed; F5 (libhalcyon's `PROG_DIRS` carries two of the shell's
five directories) and F6 (the warden's manifest name reaches the spawn path
unvalidated) are pre-existing and enqueued. The order the breakers surfaced in
is the round's lesson: a boot stops at its first failing probe and proves
nothing past it, and the reviewer read past the point the boot died. Verified
sound: the census denominators, no shared Territory, pivot and chroot leave the
dot, every failure path keeps the post-pivot reset, the `/bin` bind reaches
nothing above `bin/`, the shed keys on (dc, devno), `may_back_exec` per Dev,
and the five search lists agree.
