---
id: chg-2026-09-07-ptyfs-doc-absorb
type: chg
title: "absorb docs/reference/136-ptyfs (the pts server, I-20/PTY-2): fold 3 gaps into sub-ptyfs (cacheability fail-safe, item-10 readiness, drop_modeflush); PTY-4 #19/TTIN deferred to 135-pty-kernel"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-ptyfs]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The native pts 9P server (usr/ptyfs). Audit:hard surface (I-20 data path, PTY-2).
quaestor owner: usr/ptyfs/src/main.rs -> sub-ptyfs (dedicated, audit:hard, but
updated 2026-08-03 -- BEFORE this doc's PTY-2e item-10 + the ccb597b8 round).
Verified atom-by-atom; the age gap surfaced 3 real folds.

ALREADY COVERED (verified): the devpts tree + qid (PTS_FLAG bit40, ptsname decode,
S_IFCHR-vs-S_IFREG is-a-tty discriminator), input/output cook + SignalXorByte +
ECHO-off chokepoint + ring-full split, tcsetattr-atomic ctl + mode-write-delivers,
PendingRead multi-waiter + I-9-single-thread, drain-then-EOF + slave_opened_once +
HupAtMostOnce-by-construction + free-on-last-unref, the #13 I-1 gap (0666
SYSTEM-owned world-reach, per-pts-0600 fix fork), the #95 counters (3 of 4).

THE 3 FOLDS (genuine gaps -> sub-ptyfs, depth rich; updated 08-03 -> 09-07):
- Cacheability fail-safe: E_NOSYS dispatch default -> kernel never latches
  cacheable -> Larder caches no pts byte (a cached tty read replays stale bytes);
  a prosecuted fail-SAFE, was ABSENT (grep cacheab/larder/nosys -> 0 hits).
- Item-10 <n>ready QTPOLL bridge: the separate per-pts QTPOLL companion (offset-
  mask probe, PendingRead{probe:true} in the same Vec, the ^C-eats-next-line fix,
  pts=dev9p_poll's 2nd client after netd). WHOLE mechanism ABSENT (dossier
  predates item-10).
- drop_modeflush 4th #95 counter: sub-ptyfs listed only 3; the 4th carries #95's
  EXACT shape (short mode-flush loses the tail but delivers the terminator RAW ->
  the truncated command RUNS, unlike a short cooked flush which drops the newline
  too). Folding it into drop_flush would falsify that row (kernel twin
  rx_drop_modeflush).

DEFERRED to 135-pty-kernel (kernel-side, verified NOWHERE in vault): PTY-4 #19
resume-then-re-stop (the stop cascade reused the DEATH completing torpor_wake_all,
fabricating TORPOR_OK for a SURVIVING job-stopped Proc; fixed 2-layer:
torpor_stop_wake_all_for_proc non-completing + time::sleep re-sleeps spurious
Woken) + TTIN foreground-read-arbitration #18 -- both kernel proc/torpor, home
verified at the 135 absorption. Redirect stub. Zero code change.
