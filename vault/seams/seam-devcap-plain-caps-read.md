---
id: seam-devcap-plain-caps-read
type: seam
title: "The cap device's two grant-register gates read caps non-atomically"
status: open
surface: [sub-kernel-caps]
opened-by: chg-2026-08-02-authority-sweep
tracker: "task #15"
created: 2026-08-02
updated: 2026-08-02
---
## Owed

Convert the two gate reads in `cap_register_grant_for_writer` and
`cap_register_clearance_grant_for_writer` to
`__atomic_load_n(&writer->caps, __ATOMIC_ACQUIRE)` — the same one-line change
already applied at every other capability gate in the tree.

## The gap

`cap_register_grant_for_writer` gates on `CAP_GRANT_HOSTOWNER`, and
`cap_register_clearance_grant_for_writer` on `CAP_GRANT_CLEARANCE`. Both read
`writer->caps` with a plain load.

A census of every `->caps` read in the kernel finds these are the only two
left. `syscall.c` (sixteen sites), `devproc.c` (four), `devctl.c`, `perm.c`
(two) and `proc.c` (two) all use an acquire load, and most carry an explicit
comment — "R9 F146", "RW-5 F2", "RW-5 R3-F4" — stating that a plain load is
C11-racy now that `proc_become_legate` is a cross-thread writer of `p->caps`.

The sharpest detail is that the fix and the miss are in the **same file**.
RW-5 F1 hardened `cap_redeem_grant_for_writer`'s `__atomic_fetch_or`, and
its comment reasons carefully about why plain access to `p->caps` is no
longer sound — while the two register-gate reads above it went unconverted.
This is the "a fix on site N stops you asking about site N+1" pattern, at
its shortest possible range.

## Why it is small

On aarch64 an aligned `u64` load is single-copy-atomic, so the value cannot
tear; and the sole writer of these two gates is corvus, single-threaded at
v1.0. What is actually lost is compiler-level: nothing stops the load being
hoisted, split, or re-materialised around the `spin_lock_irqsave` that
follows, and nothing establishes the acquire edge against the releasing
writer.

So this is a discipline defect rather than a live privilege hole — but the
discipline is the thing that makes the *rest* of the sweep trustworthy, and
the gate it gaps is "who may register a capability grant".

## Risk while open

Low today; it rises the moment corvus becomes multi-threaded, or any second
holder of `CAP_GRANT_*` appears. It should land as a matter of course rather
than being weighed, because the fix is one line per site and the reasoning
for it is already written down twice in the same file.
