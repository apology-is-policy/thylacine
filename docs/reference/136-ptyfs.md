# 136 — ptyfs: the pseudoterminal server + /dev/pts (PTY-2) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-ptyfs-doc-absorb`).
`/sbin/ptyfs` — the native libthyla-rs device-less `/srv` 9P server owning the pts
pairs (two byte rings, the per-pts line discipline, the termios+winsize ctl, the
teardown semantics). It realizes **I-20's data path**; the kernel owns the
security-routing half (PTY-1). Its content lives, code-verified and current, in
its dedicated audit:hard dossier:

      vault/system/userspace/services/sub-ptyfs.md   (audit: hard, I-20)

Everything the doc carried is there — the devpts tree + qid encoding (`PTS_FLAG`
bit 40, the `ptsname` decode contract, the S_IFCHR-vs-S_IFREG is-a-tty
discriminator against netd's bit-40-but-S_IFREG qids), the input/output cook order
+ `SignalXorByte` + the ECHO-off no-leak chokepoint + the ring-full policy split,
the tcsetattr-atomic ctl grammar + the mode-write-delivers-never-discards rule,
the deferred multi-waiter `PendingRead` set + the I-9-by-single-threadedness
argument, drain-then-EOF + `slave_opened_once` + `HupAtMostOnce`-by-construction +
free-on-last-unref, the #13 I-1 access-control gap (0666 SYSTEM-owned; any Proc
naming a live pts can read/inject/re-termios; inert single-session, live under
A-5b; the per-pts-0600 fix fork), and the #95 input-drop counters — **now
including the three atoms this absorption folded in** (see below).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Three genuine gaps, now folded into sub-ptyfs** (the dossier was
  updated 2026-08-03, before PTY-2e item-10 and the `ccb597b8` round):
  - **The cacheability fail-safe** — ptyfs answers `Twalkgetattr` (and every
    unimplemented T-message) with `E_NOSYS` via the dispatch default, so the
    kernel client never latches `cacheable` and the Larder caches no pts byte
    (a cached tty read would replay stale bytes). A prosecuted fail-*safe*, was
    absent.
  - **The item-10 `<n>ready` QTPOLL readiness bridge** — the separate per-pts
    QTPOLL companion (offset-encoded mask probe, `PendingRead{probe:true}` in the
    same flat Vec, the "^C eats the next line" fix, pts as `dev9p_poll`'s second
    client after netd). The whole mechanism was absent.
  - **The `drop_modeflush` fourth #95 counter** — sub-ptyfs listed only three
    counters; the fourth is the one that carries #95's *exact* observed shape (a
    short mode-flush loses the tail but delivers the terminator raw, so the
    truncated command **runs**, unlike a short cooked flush which drops the
    newline too and never runs the line). Folding it into `drop_flush` would have
    falsified that row.
- **The PTY-4 #19/TTIN job-control content is kernel-side, deferred to
  135-pty-kernel.** The resume-then-re-stop root-cause (#19 — the stop cascade
  reused the DEATH cascade's *completing* `torpor_wake_all`, fabricating
  `TORPOR_OK` for a *surviving* job-stopped Proc so a torpor-timed sleep
  "finished" at resume; fixed two-layer with the non-completing
  `torpor_stop_wake_all_for_proc` + `time::sleep` re-sleeping a spurious `Woken`)
  and the TTIN foreground-read-arbitration gap (#18) live in the kernel
  proc/torpor + PTY-1 job-control surfaces, not ptyfs. They are documented here
  but their home is verified/folded when `docs/reference/135-pty-kernel.md` is
  absorbed. Zero code change.
