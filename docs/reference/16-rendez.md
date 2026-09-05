# 16 — Rendez (wait/wake) [ABSORBED INTO THE VAULT]

Absorbed at the scheduler sweep (`chg-2026-08-01-sched-sweep`). Its
content now lives, code-verified and current, at:

    vault/system/kernel/scheduling/sub-kernel-rendez.md

(the single-waiter Rendez, `sleep` / `tsleep` / `wakeup`, the global
timer-wait list, the register-then-observe death close, the two stop
detours, and the frame-atomic exception for the elected 9P reader),
with the lock chain at `vault/locks/lock-wait.md`,
`lock-timerwait.md` and `lock-rendez.md`.

**What this file got WRONG by the time it was absorbed — and it is the
sharpest instance of the mode found so far.**

The previous two sweeps found *stale sections* bolted onto current ones.
This file goes further: **it states the #811 rule correctly in prose
three times, and states its exact opposite in the mechanism descriptions
twice**, roughly a hundred lines apart, with nothing marking which wins.

- Line 65: "**#811: `wakeup` no longer clears the waiter's
  `rendez_blocked_on`** — only the owning Thread clears it." Correct.
- Line 183: the same rule again, in the data-structure section. Correct.
- Line 149, inside the `wakeup(r)` implementation pseudocode:
  `t->rendez_blocked_on = NULL`. **The thing the other two lines forbid.**
- Line 350, describing `wake_rendez_waiter`: "clears `r->waiter` +
  `rendez_blocked_on`". **Same error, again.**

That field is not decorative: clearing it under `r->lock` instead of the
owner's `wait_lock` races the group-terminate cascade's read, which is
what #811 exists to prevent. A reader who trusts the pseudocode — the
part that looks most like the code — gets the pre-fix protocol.

The same doubling appears elsewhere in the file:

- **The lock order is given twice, differently.** Line 75 has the correct
  `wait_lock -> g_timerwait.lock -> r->lock`. Line 339 has
  `g_timerwait.lock -> Rendez.lock -> CpuSched.lock`, and line 342 adds
  "`sleep` takes only `r->lock` — a consistent subset", which has been
  false since #811 made `wait_lock` the outermost lock of every sleep.
- **`struct Thread`'s size is given twice, differently**: "grew 200 → 208
  bytes" (line 185) and "grew 784 → 816 bytes" (line 380). It is 1232.
- The `sleep(r, cond, arg)` pseudocode (line 115) shows the whole loop
  with **no `wait_lock`, no death re-check, no stop detour** — the
  pre-#811 body — 40 lines below the section that describes #811 in
  detail.

And the ordinary staleness, for completeness:

- "at v1.0 UP the spin part is a no-op (no contention possible); at SMP
  (P2-C) it becomes the real cross-CPU contention point" — SMP landed
  2026-05-05.
- "### Deadlock detection ... P2-C lands SMP idle-WFI" — future tense for
  something landed; the extinction is now structurally unreachable
  (every CPU has a pinned in-tree idle).
- Known caveat 5: "**SMP race not yet closed (P2-Bb is UP-only)**" —
  closed at P2-Cf by the `on_cpu` handoff.
- Deferred list: "**Interruptible sleep** — when notes land at Phase 5",
  in a file whose own §"Death-interruptible sleep (#811)" describes it as
  landed.
- `Implemented: P2-Bb at <commit-pending>` — a literal unfilled
  placeholder, three months old.
- `struct Rendez`: "lock — 4 bytes — _stub at v1.0 UP".

What the file got RIGHT and is worth preserving as history: the #811
section (lines 71–78) and the whole tsleep chapter (303–441) are accurate,
detailed and were the primary source for the dossier. The problem is not
that nobody maintained it — it is that maintenance only ever *added*.

Also carried into the vault, because it is a genuine hazard and this file
did not record it: **the unconditional `r->lock` acquire in `wakeup` is
load-bearing on the no-waiter path** (PTY-4e R2). It is the only ordering
chain delivering a torpor poster's write to a stop-parked waiter's
resumed re-loop. A lockless `r->waiter == NULL` fast path there looks
like free performance and reintroduces a lost wake.

Design scripture is unchanged: `docs/ARCHITECTURE.md` §8.5, §8.8, §8.8.1,
§8.8.1.1, §8.8.2.
