# 72 — poll [ABSORBED INTO THE VAULT]

Absorbed at the memory/ipc-wake sweep (`chg-2026-08-01-mm-ipc-sweep`).
Its content now lives, code-verified and current, in:

    vault/system/kernel/ipc-wake/sub-kernel-poll.md

(register-then-observe, the hook lifecycle, the sweep's three-phase
order, the RW-2 retain discipline, the srv dispatch, the lock chain.)

**What this file got WRONG by the time it was absorbed.** The
headline is a soundness argument that was INVERTED by a later fix and
left standing: the "No fd ownership transfer" caveat — "poll does NOT
take a reference on the polled Spoors … a Spoor whose backing object
freed via some non-close path would be a UAF; **no such path exists
at v1.0**" — describes exactly the sibling-close path the
multi-thread lift created, and the absence of exactly the retain
mechanism (`held[]`, RW-2 2C-F1) that closed it. A reader trusting
the caveat would delete the fix as unnecessary.

- `waiters[]` sized by `PROC_HANDLE_MAX = 64` throughout (six-plus
  sites incl. the stack-frame caveat) — the bound is `POLL_MAX_NFDS`,
  deliberately DECOUPLED when the fd table grew to 256; restoring the
  documented identity would overflow the kstack the decouple commit
  protected. (`syscall.h`'s SYS_POLL comment carries the same stale
  claim — recorded for main-track fixing; the handler code is
  correct.)
- "No EINTR semantics yet … `poll` blocks until cond OR deadline,
  never spuriously" — false since #811; the INTR arm exists and its
  sweep-still-runs property is load-bearing.
- The Dev census ("only devpipe implements .poll") predates devsrv,
  dev9p.poll, and the cons layer; the #844 handle-snapshot API and
  `poll_waiter_list_empty` are absent.
- The retained `held[]` array, the inert-KObj_Srv-retain caveat, and
  the mortal-registry seam — the surface's live safety story — are
  absent entirely.
