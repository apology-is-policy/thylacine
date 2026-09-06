---
id: chg-2026-09-06-pipe-multiwaiter
type: chg
title: "sub-kernel-pipe brought current: the single->multi-waiter lift (two Rendezes -> one poll_waiter_list, closing the EL0-shared crash), CNONBLOCK/EAGAIN, and the item-11->11c caught-note seam"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-pipe
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
kernel/pipe.c moved ~262 lines since the dossier's 2026-08-14 update (pipe.c is
stable now, last change 2026-09-02). The churn recentred the whole concurrency
model, and the dossier's "single-waiter" framing -- Data structures, Concurrency,
Invariants, Seams -- was stale throughout. Verified against pipe.c/pipe.h.

## The single->multi-waiter lift (dd9f9508 + holotype 52657afe)

The pipe used two single-waiter Rendezes (one per direction), whose "a second
sleeper on the same rendez extincts" rule was fine while pipes were kernel-only.
It became an UNPRIVILEGED EL0 CRASH the day two threads of an EL0 Proc blocked on
the same end -- the object-embedded-Rendez hazard (a "fine in-kernel" primitive is
a crash the moment the object is EL0-shared; sweep every object-embedded Rendez).
The fix routes every blocked reader and writer through the ring's existing
`poll_waiter_list` -- the SAME list pollers use -- so any number may sleep on
either direction. The header shrank 88 -> 56 bytes (two Rendezes -> one 16-byte
`poll_list` at offset 40; the size assert is now `56 + PIPE_BUF_SIZE`), the ring
alloc is 4152 bytes (still order-1). The wake is one `poll_waiter_list_wake` per
edge. Rewrote Data structures, Concurrency (the core reframe), Mechanism (the
sleep/wake now register on `poll_list` + return SLEEP_OK re-sample / SLEEP_INTR),
Invariants (`SingleWaiter` is RETIRED -- the property it named was the constraint
the lift removed), and Seams (the old "multi-waiter never needed -- poll covers
it" was wrong twice: it WAS needed and is now BUILT). Dropped `lock-rendez` from
the frontmatter -- pipe.c's only Rendez reference is a comment explaining why not.

## CNONBLOCK / EAGAIN (34ff46df, the viv git-stash non-blocking pipe)

read/write return `-EAGAIN` in the would-block case under `CNONBLOCK`, placed
after the drain and EOF checks so only the block converts, and it never registers
a hook (the I-9 wait/wake protocol is untouched). Added to the Contract + Error
paths.

## The item-11 -> 11c caught-note seam (4b3d4e6a)

11b-core landed the caught-note MECHANISM, but a pipe read still uses plain
`sleep`, not `sleep_noteintr`: only DEATH interrupts it (the #811 die-check;
re-looping on a caught note would re-register and re-INTR, a livelock). Opting in
is deferred to 11c together with native/phenotype EINTR handling -- a native
reader (libthyla-rs, not EINTR-aware) would break on an early EINTR (e.g. `ut`'s
`$(cmd)` capture read interrupted by the captured child's `child_exit`). Recorded
as an OPEN seam (`design_caught_notes_do_not_interrupt_waits`).

`updated:` -> 2026-09-06; guarded-by unchanged [inv-i9]; audit: hard -- every
claim re-derived from the code.
