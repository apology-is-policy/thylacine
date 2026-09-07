---
id: sub-kernel-pipe
type: sub
parent: moc-kernel-ipc-wake
title: "pipe — the connected Spoor pair over a shared ring"
code: ["kernel/pipe.c", "kernel/include/thylacine/pipe.h"]
audit: hard
guarded-by: [inv-i9]
validated-by: [spec-pipe, gate-smp]
locks: [lock-pipe-ring, lock-poll-list]
created: 2026-08-01
updated: 2026-09-06
---
## Purpose

Plan 9 `pipe(fd[2])`: two Spoors over one kernel ring, FIFO bytes,
blocking both directions, EOF propagation on close. The shell
pipeline primitive, the `SYS_PIPE` (= 8) substrate, and the
byte-transport under the 9P spoor-transport adapter.

## Contract

- `pipe_create(&rd, &wr)` — all-or-nothing; both Spoors ref=1, ring
  ref=2 (one per endpoint). `SYS_PIPE` installs both as `KOBJ_SPOOR`
  handles with `R|W|TRANSFER` on BOTH ends — the wrong-end gate lives
  in the Dev (`is_read_end`), not the rights.
- **read**: drains 1..n when data is buffered; **blocks on the
  `poll_list`** when empty and the write end is open; returns 0 (EOF)
  when empty and `write_eof`; `-EAGAIN` when empty, open, and `CNONBLOCK`
  (placed after the drain and EOF checks, so it converts only the
  would-block case and never registers a hook); -1 on wrong end /
  `SLEEP_INTR` (#811 death).
- **write**: appends 1..n (short when the ring fills mid-write);
  **blocks on the `poll_list`** when full and the read end is open;
  `-EAGAIN` when full, open, and `CNONBLOCK`; -1 (EPIPE) when `read_eof`
  — and synthesizes the `pipe` note to the writing Proc (13a; the note is
  informational, the -1 is the load-bearing EPIPE musl translates).
- **close**: sets the EOF flag under `r->lock`, drops it, wakes the
  OPPOSITE rendez, then wakes the poll list — the close is a
  readiness edge (surviving read end → POLLHUP; write end → POLLERR).
  Then the atomic ring-ref drop; last one frees.
- Writes ≤ `PIPE_BUF_SIZE` (4096) fill available space; the POSIX
  PIPE_BUF framing.
- `.poll` (`devpipe_poll`): sample + register atomically under
  `r->lock` — the canonical register-then-observe implementation.
  Read end: POLLIN on bytes, POLLHUP on `write_eof`. Write end:
  POLLOUT on room-and-open, POLLERR on `read_eof`.
- `.stat_native` (#96, [[chg-2026-07-29-96-pipe-fstat]]):
  `T_S_IFIFO | 0600`, size 0 (a buffered-count report would invite a
  read sized against it, racing the peer by construction), blksize
  4096, and a monotonic `qid.path` stamped into BOTH ends at create
  (one pipe, one inode; starts at 1 so the historical unset 0 stays
  distinguishable). `.seekable` stays false — fstat-able ≠ seekable
  (RW-4 R2-F2 decoupling, pinned by `sys_prw.pipe_not_seekable`).

## Mechanism

Read and write are lock→check→act-or-sleep loops. The sleeping arm
registers a `poll_waiter` on `poll_list` under `r->lock`
(`pipe_block_locked`), then `sleep(&priv, pipe_waiter_ready, &pw)` drops
the lock and blocks; on wake it unregisters and reads `sleep`'s verdict
— `SLEEP_OK` means re-sample (another waiter may have taken the edge, so
the loop re-checks under the lock), `SLEEP_INTR` means a death-interrupt
and the op returns -1. The acting arm drops `r->lock` BEFORE
`poll_waiter_list_wake`, which walks every hook — pollers and blocked I/O
alike — under the list lock. The cond (`count > 0 || write_eof`; `count
< CAP || read_eof`) is `pipe_waiter_ready`, evaluated under the list lock
that orders the producer's mutation before it. The four wakes still map
one-to-one onto [[spec-pipe]]'s four buggy configs: delete any one and
its NoStuck invariant produces the counterexample.

Ring ops are two-segment mod-arithmetic copies; `count`/`head`/`tail`
only ever move under `r->lock`.

`pipe_create`'s rollback ladder is deliberately asymmetric at the
last rung: if the second Spoor alloc fails, the first Spoor's `aux`
is DETACHED before `spoor_clunk` so `devpipe_close` sees no priv and
never decrements the ring ref — then everything is freed manually.
A partial-failure path never exercises the close path's ref logic.

## Data structures

`struct pipe_ring` — **56-byte header + 4096 buf, size-pinned**:
magic, atomic `ref`, count/head/tail, two EOF flags, `r->lock`, and ONE
embedded `poll_waiter_list` (`poll_list`, offset 40) — the whole waiter
story, pollers and blocked readers/writers alike. (It was an 88-byte
header with two single-waiter Rendezes until the multi-waiter lift
replaced both with the 16-byte list.) The ring is kmalloc'd — 4152 bytes
routes through the large path as an **order-1 (8 KiB) allocation**,
~4 KiB slack per live pipe.
`struct pipe_endpoint` — 16 B, SLUB-cached, `{magic, ring,
is_read_end}`. Diagnostics: `pipe_total_allocated/freed` (ring-level).

## Concurrency

- The ring ref is `__atomic` ACQ_REL (r15-b F234,
  [[fnd-r15b-f234]]): two CPUs closing the two endpoints
  concurrently raced the plain `--` — lost-update or both-see-zero
  (double-free). `fetch_sub` pre == 1 owns the free; pre <= 0
  extincts.
- **Multi-waiter, on one list (the single-waiter lift).** Every blocked
  reader and writer registers a `poll_waiter` on the ring's `poll_list`
  — the SAME list pollers use — so any number may sleep on either
  direction. This replaced two single-waiter Rendezes whose "a second
  sleeper extincts" rule was fine while pipes were kernel-only and became
  an **unprivileged EL0 crash** the moment two threads of an EL0 Proc
  blocked on the same end (the object-embedded-Rendez hazard: a "fine
  in-kernel" primitive is a crash the day the object is EL0-shared). The
  wake is one `poll_waiter_list_wake` per edge, rousing pollers and
  blocked I/O together.
- The poll-list wake runs AFTER `r->lock` drops on every edge —
  the register/sample side holds `r->lock`, so a concurrent register
  either precedes the mutation (the wake finds its hook) or follows
  it (the sample sees the new state).

## Invariants enforced

[[inv-i9]] specialized to the two-direction state machine —
[[spec-pipe]]'s `NoStuckReader`/`NoStuckWriter`, now carried on the
multi-waiter `poll_list` rather than a single-rendez slot. `EofMonotonic`
pins EOF ordering. The old `SingleWaiter` invariant is **retired** by the
multi-waiter lift: the property it named — never two sleepers on one slot
— was the very constraint the lift removed, not one to keep proving.

## Error paths

-1: NULL/corrupt priv (endpoint magic extincts — UAF, not an error),
wrong end, negative len, `SLEEP_INTR`. `-EAGAIN`: a `CNONBLOCK` read/write
that would have blocked. 0: EOF (read) or len ≤ 0. Close extincts on ref
underflow or corrupt ring magic.

## Performance

O(n) byte copies, mandatory. Two lock pairs per op (ring + the
opposite wake's rendez) plus the poll-list walk when pollers are
registered.

## Prosecution

- Every mutation that can enable a waiter must keep its wake — the
  four spec buggy configs are the executable list.
- The close-order (flag under lock → drop → opposite wake → poll
  wake → ref drop) must hold; waking before the flag is visible loses
  the edge, dropping the ref before the wakes frees the rendez under
  the waker.
- `.seekable` must stay false and `size` must stay 0 (#96's two
  pinned properties).
- The rollback ladder's aux-detach must precede the clunk.

## Seams

- **A pipe read is not yet caught-note-interruptible (item 11 → 11c).**
  11b-core landed the caught-note *mechanism*, but a pipe read still uses
  plain `sleep`, not `sleep_noteintr`: only DEATH interrupts it (the #811
  die-check; re-looping on a caught note would re-register and re-INTR, a
  livelock). Opting in is deferred to 11c, which lands it together with
  native/phenotype EINTR handling — returning EINTR before a native
  reader (libthyla-rs, not EINTR-aware) can cope would break it, e.g.
  `ut`'s `$(cmd)` capture read interrupted by the captured child's own
  `child_exit`. See `design_caught_notes_do_not_interrupt_waits`.

The pouch `pipe(2)` translation landed long ago. The "multi-waiter
direction queues (never needed — poll covers it)" this section once
claimed was wrong twice over: it WAS needed (the EL0-shared crash above)
and it is now BUILT (the single-waiter lift).

## Caveats

- `docs/reference/51-pipe.md` (absorbed) shows the pre-blocking
  struct fields, pins the size at 72+4096 in prose, and reports the
  allocation as "order-2 = 16 KiB, 12 KiB waste" — three eras of
  wrong for a pinned struct (actual 88+4096, order 1, ~4 KiB slack),
  while `72-poll.md` next door documented 88 correctly. Its
  Performance section still says "No locking at v1.0 (single-CPU)"
  two screens above the Status row recording the lock. And
  `kernel/include/thylacine/pipe.h`'s OWN header block still
  describes the P5-pipe non-blocking semantics ("neither end blocks")
  as current with blocking as future — a code header inverted by the
  next chunk, never updated.
- `52-sys-pipe.md` (absorbed) is frozen at P5-fd-pipe: "userspace can
  `pipe()` but can't actually use the fds", "`uaccess_store_u32`
  doesn't yet exist", `PROC_HANDLE_MAX = 64`. That last one is **1024**
  today — 64 → 256 at the go-arc growth → 1024 at the #198 fid-ceiling
  chain — and it is the bound `sys_pipe_for_proc`'s `handle_alloc`
  failure arm reports against, two fds at a time. The stub's own
  correction of it said "256" and needed re-correcting inside a month,
  so it now records the sequence rather than a value.

## Provenance

[[chg-2026-05-14-p5-pipe]] (primitive → blocking + [[spec-pipe]]) →
[[chg-2026-05-14-r15b-atomic-refs]] (F234) →
[[chg-2026-05-20-p5-poll]] (`.poll` + the wake callouts) → 13a
(`notes_post_pipe`) → #811 INTR arms →
[[chg-2026-07-29-96-pipe-fstat]] (fstat + qid identity, the CL-5
build-storm door) →
[[chg-2026-09-06-pipe-multiwaiter]] (the single→multi-waiter lift that
retired the two Rendezes for one `poll_waiter_list`, closing the
EL0-shared crash; `CNONBLOCK`/EAGAIN; the item-11→11c caught-note seam).
