---
id: chg-2026-08-02-async-sweep
type: chg
title: "vault sweep: the async area -- rings, shared pages, and private copies"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-loom
  - sub-kernel-weft
established:
  - inv-i29
  - inv-i30
  - inv-i37
closed: []
opened:
  - seam-f-notif-unwired
  - seam-loom-rearm-needs-blocking-enter
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 17. Read from code: `kernel/loom.c` (2102), `loom.h` (602), `kernel/weft.c`
(673), `weft.h` (565), plus the three weft syscall inners + `burrow_share_into`'s
cross-Proc proof + the six spec modules' invariant sets and 36 configurations.
Two dossiers under `system/kernel/async/`, declared-and-empty since commit 0.

WHY THIS BATCH. The largest remaining audit-bearing area, and the home of three
unminted invariants (I-29, I-30, I-37) -- so it unblocks the registry pass by
more than any other. Loom carries five audit rounds (Loom-2 through 6d), Weft
one plus two graphics-era addenda.

THE ORGANIZING FACT is a one-line rule that accounts for ~15 separate decisions
across the two files:

    A shared word may BOUND the work. It may never LOCATE the memory.

Every shared control block has a kernel-private twin that is authoritative; the
shared copy is a MIRROR the kernel publishes. The user's own words ARE read --
the submission tail bounds the drain, the completion head computes fullness --
and that is safe precisely because neither can produce an index. Every actual
index is `private_counter & private_mask`. A hostile completion head only lets a
Proc overwrite its own unreaped result, in its own region.

There is EXACTLY ONE user-written word that reaches an index (the submission
ring's indirection slot, which names an entry), and it is range-checked against
the private entry count before it indexes anything. The exception that shows the
rule is deliberate rather than incidental.

THE DEFERRAL SHAPE, FOR THE THIRD CONSECUTIVE AREA. The completion callback runs
under the engine's lock and may not sleep or re-enter it, so re-arm and chain
admission are flagged and handed to a drive loop. Same STRUCTURE as batch 15
(an interrupt handler may not walk the poll list -> a manager kthread does) and
batch 16 (a handler may not deliver a note mid-exception -> the return tail
does), with a different REASON each time.

Naming it paid immediately, because the bug class travels with the pattern: the
interesting failure is never the deferral, it is a context that SHOULD run the
deferred work and does not. Batch 16 found that in the entry area (one of two
return tails missing the notes hook, task #21). Looking for the same shape here
found [[seam-loom-rearm-needs-blocking-enter]]: re-arm runs in the wait loop and
the poll thread's loop, but NOT in the submit phase -- so on a ring without a
poll thread, a call that submits without blocking never resumes a multishot
stream. Held, not lost; and NO consumer can reach it today (every payload opcode
rejects multishot; only the synthetic durability-barrier vehicle accepts it).
The ORDERING consequence of the same asymmetry was already found by an audit and
closed by adding a term to the barrier gate -- the code says outright that "the
submit-phase admission has no preceding re-arm". The LIVENESS consequence was
never stated.

THE OTHER FINDING: A DEFENCE WITH NO CALLER. [[inv-i37]]'s third clause -- a page
in flight is never reused -- names the multi-holder tracker as its defence. The
tracker is complete, modelled ([[spec-weft]]'s PinHeldWhileInFlight /
NoInFlightReuse + the premature-release counterexample), and unit-tested. Its
five entry points have callers ONLY in the test file; nothing arms a holder, and
the notification completion flag is defined but never set on any path.

The system is safe anyway: netd COPIES the ring into its socket buffer, so no
page is ever in flight past its reply. But the safety comes from somewhere other
than where the invariant says it does -- from a userspace daemon's decision to
copy, in another layer -- while the invariant names a kernel tracker that never
runs. The clause holds VACUOUSLY, by avoidance rather than by defence. That is
fine today and matters at the next change: the moment netd holds the page
instead of copying, the property stops holding by itself and the tracker must be
wired onto the in-flight op with the pin release moved from reap to
notification-terminal. Both described; neither exercised in composition.
[[seam-f-notif-unwired]]. The forward-compatible half is already right (a
consumer decides whether to wait by reading the "more follows" flag, which is
clear on today's copied path and set on a deferred one) -- so consumers written
against current behaviour already ask the right question.

MEASURED, NOT COUNTED BY EYE. 15 of the 20 opcodes dispatch; the 5 that do not
are exactly the fid-LIFECYCLE ops (walk / open / create / clunk) plus the
reserved passthrough -- coherent, not arbitrary: registered handles wrap
already-open fids, so those five need a registered-slot install/release surface
that does not exist. Three Tweftio fast-path consumers, matching the scripture's
claim that one kind-gate closes all of them. 80 registered tests across the area
(25 loom.* + 27 weft.* + 28 9p_client.loom_* -- the last group living in the 9P
test file because they drive the multi-in-flight transport).

THE STALE-SUMMARY CLASS, THIRD CONSECUTIVE BATCH, FOUR MORE INSTANCES. All four
files describe their FIRST sub-chunk. `loom.h`'s status block says "the ring
substrate ... **no op flows yet** -- the opcodes are reserved ABI" above a file
that dispatches fifteen, and lists work through Loom-3 as future; `loom.c`'s
opening line calls itself the ring substrate and says dispatch and completion
posting live elsewhere (they are IN it); `weft.h` says "this header is the
Weft-3 SUBSTRATE" and lists the delivery calls as future, while carrying the
share registry, the weave kind and the reaper; `weft.c` says the same and names
the delivery "Weft-6" -- the registry is in that file.

Ten instances across three batches now, every one in file-level summarizing
prose, none in a per-construct comment. And the contrast here is stark: these
files' per-function comments are the BEST in the tree, carrying audit finding
references ("6c audit F1", "R2-F1", "Weft-7 F1", "G-3-audit F1") at the exact
lines that fixed them. The drift is not carelessness -- it is that a fix updates
the comment beside it and nobody re-reads the top of the file.

Also noted: an overflow-safety comment in the ring geometry over-estimates the
completion array at twice its real maximum. Fail-safe direction (the bound it
proves still holds), but it is arithmetic in a comment whose only job is that
arithmetic.

REGISTRY TAIL. Minted this batch's own dependencies and stopped: three
invariants and the six spec notes their `validated-by` names (loom, multishot,
order, devgone, weft, readiness). The three sibling Loom modules exist because
the base module's completion queue is a SET -- one entry per op is baked into
its state shape -- so a stream could not be expressed without invalidating its
eight landed configurations; the sched_oncpu precedent.

I-5 deliberately NOT minted, though Weft upholds it (device-interpreted regions
stay structurally unshareable): its `guards` home is the handle table, and
minting it here would misfile it. Recorded in prose as a claim this area upholds
rather than owns -- the batch-16 treatment of I-13's separation half.

SCOPE. `usr/lib/libthyla-rs/src/loom.rs` + `net.rs`'s WeftFlow are the userspace
mirrors and stay with a future userspace sweep; `burrow_share_into` was read for
the cross-Proc dual-refcount proof but stays owned by the memory area. The
graphics consumer of the weave kind (tapestryd) is its own unswept surface.
