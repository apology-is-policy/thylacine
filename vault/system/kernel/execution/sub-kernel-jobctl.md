---
id: sub-kernel-jobctl
type: sub
title: "Job control — the second owner of the stop park, the catchability gate, and the POSIX orphan rule"
parent: moc-kernel-execution
code: [kernel/proc.c, kernel/include/thylacine/proc.h]
audit: hard
guarded-by: [inv-i20, inv-i39, inv-i9, inv-i19]
validated-by: [spec-pty-stop, spec-debug-stop, prose, gate-smp]
locks: [lock-proc-table, lock-wait]
hazards: []
abis: []
design: ["docs/PTY-DESIGN.md section 4"]
created: 2026-08-03
updated: 2026-09-05
---
## Purpose

Ctrl-Z. A foreground process group stops; the shell takes the terminal back;
`fg` resumes it. The machinery is about three hundred lines inside the process
file, and it exists because the debugger got there first.

The debugger already had a stop: a per-Proc flag, threads parking at their
EL0-return checkpoint, a resume that clears the flag. Job control needs the
same park for an entirely unrelated reason. The whole difficulty is that
**one park now has two independent owners**, and the obvious implementation —
one flag — is wrong in a way no single-owner test can detect.

## Contract

| Function | Contract |
|---|---|
| `proc_job_stop_pgrp(pgid)` | the suspend fan: per member, decide caught-vs-stop, return the affected count |
| `proc_job_cont_pgrp(pgid)` | the continue fan: post the note and resume every alive member |
| `proc_job_stop_proc(m)` / `proc_job_cont_proc(m)` | the single-target `/proc` verbs — **unconditional**, no note, no gate |
| `proc_orphan_rule_locked(dying)` | at every death, hangup-then-continue any group this death newly orphans |
| `proc_pgrp_in_session(pgid, sid)` | the membership gate the terminal calls run *before* locking |
| `proc_job_stop_self(m)` | the **self-stop**: apply a *caught* suspend's default STOP to the caller — no note, no re-run of the catchability gate, gated instead on freshness + not-orphaned |

The two fans are called from the terminal seam ([[sub-kernel-pts]]); the two
single-target calls from `/proc/<pid>/ctl` ([[sub-kernel-devproc]]); the orphan
rule from the zombie chokepoint ([[sub-kernel-death]]); the self-stop from the
note path ([[sub-kernel-notes]]) — both `SYS_NOTED(NDFLT)` on a suspend note and
the delivery of a `tty:susp` a masking process opted to receive.

## Mechanism

### Two owners, two flags

A thread parks iff `debug_stop_req | job_stop_req`, and **each resume clears
only its own flag**. A continue can never run a debugger-stopped thread; a
detach can never release a Ctrl-Z.

The job flag physically occupies the padding slot after the debug flag — same
cache line, so the return-tail fast path that reads both pays for one line.
That is a deliberate placement, asserted at its offset, and it means adding
the second stop owner cost the Proc structure nothing.

Neither flag propagates across `rfork`. A spawned child is neither being
debugged nor Ctrl-Z'd.

[[spec-pty-stop]] exists for exactly this composition, with a buggy
configuration whose only fault is a resume that clears both flags.

### The catchability gate

A suspend character does not simply stop the group. Per member:

- **Caught** — the member has an async handler, or manages its own notes via
  a notes fd, or every one of its threads masks the terminal note family. The
  note is posted and delivered on the member's terms; **no stop**.
- **Uncaught and resumable** — the default stop fires, and *consumes* the
  signal. No note is posted.
- **Uncaught and orphaned** — discarded entirely. Nothing posted, nothing
  stopped.

The first branch is not a nicety. Terminal multiplexers and shells catch
suspend to save terminal state before yielding it, and an uncatchable suspend
would fail the project's own terminal-emulator goal.

The third branch is POSIX's stop-suppression rule and its reasoning is
mechanical: an orphaned group has no process left that could resume it, so
stopping it is stopping it forever.

**The gate runs at post time, not at the return tail** — which is the reverse
of the uncaught-interrupt terminate, whose decision is deferred to delivery.
The asymmetry is because the stop is *applied* by the poster and the terminate
is applied by the target.

A handler registered concurrently with the decision orders before or after it,
indistinguishable from the signal arriving a moment earlier — the ordinary
POSIX signal race, and the reason the reads here are lock-free.

### One reschedule for the whole group

Setting the flag is not enough: a member running at EL0 on another CPU has to
trap somewhere to notice. Each member's *own* sleepers are woken as it is
stopped, but the cross-CPU kick is a **broadcast**, so the fan issues exactly
one after the walk rather than one per member — and only if a member actually
stopped, since a pure note fan needs no kick.

The periodic tick is the floor if the kick is missed, which is the same
delivery argument the group-termination path rests on.

### The wake that must not complete

Waking a stopped process's sleepers is not the same as waking a dying one's.

The death cascade fabricates a *completed* wait — harmless for a Proc that is
about to stop existing. Applied to a stop it was a real bug: the fabricated
success rode back to EL0 at resume and made every timed wait *finish* instead
of continue, so a resumed `sleep` exited, `fg` reported the job done, and a
second Ctrl-Z found no job at all.

The stop path therefore uses a **non-completing** wake: the waiter is roused,
finds its wait unsatisfied, re-loops into the stop detour, and parks with the
wait preserved. On resume it re-registers with its original deadline. Parks
and re-parks — the same shape Linux uses for a stop over a futex wait.

### The report latches

A parent waiting with the untraced or continued options learns about a stop or
a resume without reaping. Two booleans on the Proc, set under the process-table
lock, consumed by the wait scan under that same lock — so the register-then-
observe argument is the zombie wake's, unchanged.

**Each latch supersedes the other.** A stop clears any unreported continue and
vice versa, so a parent that missed an edge sees the *current* state rather
than a queue of stale ones. And a second suspend on an already-stopped process
is discarded without re-arming the latch, which is POSIX.

### The `/proc` verbs are a different animal

`proc_job_stop_proc` and its resume take a single Proc and are **unconditional**:
no note, no catchability gate. A `/proc` stop is Plan 9's `stop` — uncatchable
exactly as the `/proc` kill beside it is uncatchable, and authorized by the
same two-axis gate, on the argument that stopping is strictly weaker than the
killing that gate already permits.

They share the per-member primitives with the terminal fan, so the report
latches still fire and the parent still sees the edge — correct, because
whoever stops a process, it is the *parent* whose wait reports it.

### The self-stop — a process that caught the suspend and asked for it anyway

The catchability gate's *caught* branch posts the suspend note and stops
nothing; the process now holds the note and decides. A shell that catches it to
save terminal state, having saved it, then wants the default action — the stop
it dodged. It gets there by `SYS_NOTED(NDFLT)` (or, for a process that merely
masks the family and opted to receive the note, the note's own delivery), and
that lands in `proc_job_stop_self` — a **third stop source** distinct from both
the terminal fan and the `/proc` verbs, called from [[sub-kernel-notes]].

Its defining property is what it must *not* do: **it does not re-run the
catchability gate.** The gate's question — "does this process catch the note?" —
is already answered *yes*; re-asking it here would read the handler address,
conclude "caught", and refuse the very stop the handler just requested. That is
the ignore bug the `#15` work names explicitly.

So the self-stop replaces the gate with two premises of its own, both read under
the process-table lock so a continue cannot land between the check and the stop:

- **Freshness** (`susp_stop_armed`, the `#240` guard): a continue that overtook
  this suspend cleared the flag, so a stale `NDFLT` arriving after the job was
  already resumed stops nothing. This is the self-path analog of the report
  latches' mutual supersession — a disposition decided at *post* time, applied
  at *noted* time, and the world may have moved in between.
- **Not orphaned** (`pgrp_orphaned_locked`): the same POSIX stop-suppression the
  fan applies, re-checked here because the fan's check was at post time and the
  group may have been orphaned since — carrier loss (the terminal gone though
  the shell lives) as well as shell death.

If it stops, it reuses `proc_job_stop_one_locked` — so the report latches fire
exactly as the fan's do — and issues the one reschedule broadcast for peers
running at EL0 on other CPUs (the caller itself parks a few instructions later,
at its own EL0-return tail).

### The orphan rule

POSIX 2.4.3: when a process group becomes newly orphaned and has stopped
members, each member gets a hangup followed by a continue. It runs at the
zombie chokepoint, **before** the reparent, with the dying Proc excluded from
every walk as already-dead — so the question asked is "orphaned once I am
gone", which is precisely what the reparent to init is about to make true.

A death can newly-orphan two disjoint things, and both are checked:

- **Each distinct group among its alive children** — but only if the dying
  Proc itself anchored that group. A non-anchoring parent's death changes
  nothing, and re-signalling an already-orphaned group would deliver a
  spurious and possibly lethal hangup.
- **Its own group**, if its own parent edge was that group's anchor.

*Anchored* means: some alive member has an alive parent in the same session
and a different group — a shell-shaped process that could still resume it.

Children are deduplicated by first-sibling-with-this-group, without allocating,
which the child cap makes safe. The walks are linear in the process count per
candidate; death is not a hot path and the fan is the rare case.

The per-member order is hangup, then the terminate wake, then continue, then
the job resume. The middle step is what makes an uncaught hangup's termination
actually run: the hangup arms the terminate latch, the wake unwinds the
member's blocked threads to die at their tails, and a stop-parked thread's
park loop bails on the pending death and dies from inside the stop.

**Death wins from inside a stop** — the same clause the debugger's stop must
satisfy, restated against the second owner. Without it, killing a Ctrl-Z'd job
would hang forever.

## Data structures

No structures of its own. Four fields on the Proc — the job stop flag, the
freshness guard the self-stop reads (`susp_stop_armed`), and the two report
latches — plus five small stack-allocated walk contexts. All four sit in the
same tail-pad region as the debugger's stop flag, so the second stop owner cost
the structure nothing. Every walk is a callback over the process tree under one
lock hold, with no allocation anywhere on the path.

## Concurrency

Everything runs under [[lock-proc-table]], which pins group membership, the
parent edges, and the thread lists for the duration of a fan. The per-thread
[[lock-wait]] nests under it for the wake walks — the established order, shared
with the death cascade.

The stop and resume flags are release-stored and acquire-loaded, matching the
debugger's flag discipline exactly, because the park predicate reads both.

The catchability gate reads the handler address, the process flags, and each
thread's note mask lock-free. The thread walk is pinned by the caller's lock;
the mask is owner-written, so the cross-thread load is a benign race made
explicit.

**One predicate, two polarities.** The self-managing-notes query fails closed
toward *not* self-managing, and its comment justifies that as the safe default
for the uncaught-interrupt terminate — an unverifiable Proc must not dodge
being terminated. Here the same answer pushes toward *stopping*. Both land on
"act on it", so the default is safe in both, but only one of the two reasons is
written down. A fail-closed default is closed relative to a particular
question, and this predicate now answers two.

## Invariants enforced

[[inv-i20]]'s stop leg, and it is the composition rather than the stop that is
load-bearing: `StopCompatI39` — a debug stop persists until its own resume, and
a job resume never clears it, and the converse.

[[inv-i39]] survives unchanged: a job stop confers no debug readability, and
the debugger's fully-stopped predicate deliberately still means *debug*-stopped
only.

[[inv-i9]] on both the wake cascade and the report latch — register-then-
observe under the same lock the observer re-scans under.

[[inv-i19]] on the note posts, which run through the ordinary delivery path.

## Error paths

The fans do not fail. They return counts — members affected, members visited —
and a zero group id returns zero. The single-target calls are idempotent by
their guards: a second stop or a resume of a running Proc is a no-op, and the
boot Proc is never a stop target.

The membership gate returns a plain boolean and treats a zero group as absent.

## Performance

Every fan is a full process-tree walk under the global lock. The orphan rule
is worse — up to two walks per candidate group per death. Both are justified by
their frequency: a suspend is a keystroke, and the orphan fan requires a death
that newly orphans a group *with stopped members*, which is rare.

The one place frequency was engineered is the reschedule broadcast, hoisted out
of the per-member loop.

## Prosecution

- The two stop flags must stay separate, and each resume must clear only its
  own. This is the invariant the sibling model exists to hold.
- Death must keep winning from inside a stop, on every path.
- The catchability gate's three outcomes must stay exhaustive and mutually
  exclusive — in particular, the uncaught-and-orphaned branch must keep
  discarding rather than falling through to a stop.
- The orphan rule must keep excluding the dying Proc from every walk, and must
  keep firing only for *newly* orphaned groups — re-signalling an
  already-orphaned group is a spurious and possibly lethal hangup.
- The stop wake must stay non-completing. A completing wake is the shape that
  silently finishes timed waits.
- The report latches must keep superseding each other rather than queueing.

## Seams

**A group stopped through a terminal and then displaced from the foreground**
— the ordinary sequence after a Ctrl-Z, since the shell re-seats itself — is
not tracked by provenance. It is covered by composition: the shell receives
the teardown hangup; if it dies, the orphan rule reaches its newly-orphaned
stopped groups; if it survives, its slave fd still resolves and the resume
path still works. Once the terminal entry is freed, that last leg dies with
it, and the hangup-surviving-shell-with-stopped-jobs corner needs a
kill-authority continue.

## Caveats

**The design's motivation and the built mechanism differ on leader death.**
See [[sub-kernel-pts]]'s caveats and task #68 — the orphan rule is what exists,
and it is narrower than the POSIX rule the design cites.

**Only the terminal path consults the catchability gate.** The other two stop
sources bypass it, for opposite reasons. The `/proc` verb never asks because a
`/proc` stop is unconditional by design. The self-stop never asks because the
gate was *already* asked and answered yes — re-asking would read the live
handler, conclude "caught", and refuse the very stop the caller requested via
`NDFLT`. All three reuse the same per-member helper, so the choice of shape is
not derivable from the primitives: a fourth stop source would have to make it
afresh, exactly as the self-stop did when it chose freshness-plus-orphan over
the gate.

## Provenance

[[arc-pty]] sub-chunks PTY-1e (the report latches and the wait selectors) and
PTY-1f (the stop, the catchability gate, the orphan rule, the resume), with the
single-target verbs arriving from the process-monitor tool. The suspend-over-
timed-wait fix is later, from the terminal arc's own shakedown.

## Tests

`proc.job_stop_owner_algebra` is the composition — the two flags and the four
combinations of stop and resume. `proc.job_stop_park_report_cont_live` and
`proc.wait_pid_for_report_not_reap` / `proc.wait_pid_syscall_untraced_flag`
cover the latches. `proc.job_stop_preserves_torpor_wait` is the
non-completing-wake regression, and it is the sharpest test here: it asserts a
timed wait *survives* a stop, which is the property whose absence produced a
visible shell bug rather than a crash.

`proc.job_stop_orphan_rule` carries both polarities in one body — a suspend on
an orphaned group affects nobody and posts nothing, and then the *same*
suspend on the same group, once re-homed under an anchoring parent, stops it.
Two outcomes from one stimulus with only the anchoring changed, which is what
makes it a test of the rule rather than of the fan.

From the terminal side, `pts.tty_tstp_stop_cont_seam` and
`pts.teardown_hup_cont` drive the same machinery through the real entry
points.

## Referenced by

[[sub-kernel-pts]] (the suspend and continue fans arrive from there) ·
[[sub-kernel-death]] (the orphan rule hooks the zombie chokepoint) ·
[[sub-kernel-proc]] (the session and group fields, and the wait scan that
consumes the latches) · [[sub-kernel-devproc]] (the single-target verbs) ·
[[inv-i20]] · [[spec-pty-stop]].

[[chg-2026-08-15-stale-by-cotenancy]] re-verified this dossier without changing it.
Its two files moved ~1440 lines in the interval; a word-bounded diff over every
job-control token found **zero semantic changes**. `kernel/proc.c` had none at
all, and every hit in `proc.h` was an offset shift — the session and group ids
moved from 336/340 to 304/308 because the address-space extraction shrank the
struct out from under them.

One detail from that shift is worth keeping, because it inverts the usual
lesson: a *summary* assertion message that had spelled the offsets out
numerically was rewritten to name the fields without the numbers, while the
individual per-field asserts were updated to the new values. Dropping a number
is normally how a proof goes stale — here it removed the only copy that had to
be maintained by hand, three lines above the copies that are checked by the
compiler.

**2026-08-16: flagged by co-tenancy, nothing owed.** `kernel/proc.c` and its
header moved ~37 lines and this dossier was flagged for it. Checked by hunk
context against the function set it owns — sessions, process groups, the group
note fan, the terminal seam, the job-control stop: **none was touched.** Every
hunk landed in `proc_exec_alone` / `proc_exec_replace` /
`proc_exec_reset_dispositions`, which are [[sub-kernel-proc]]'s
([[chg-2026-08-16-proc-exec-ledger]]). The same disposition
[[chg-2026-08-15-stale-by-cotenancy]] recorded for the capability surface,
which shares the same file.

**2026-09-05: post-08-16 churn borrowed; a pre-existing self-stop gap closed
([[chg-2026-09-05-jobctl-self-stop]]).** `proc.c` moved ~1097 lines since and the
dossier was flagged again. The owned entry-point set was checked function by
function against the current tree with git's function-aware `-L`: all six are
unchanged since 2026-08-14, predating the last update. The one adjacent
post-08-16 change is `proc_mark_self_managing_notes` gaining a Design-D
exec-clear (the mark is the image's, dropped at every execve) — that is the
mark's *lifecycle*, owned by [[sub-kernel-proc]] / [[sub-kernel-exec]]; this
dossier describes only the *read polarity* of the query, which is unchanged.
Borrowed, exactly as the two entries above.

But the surface review found a gap that *predated* the dossier: the `#15`/`#240`
self-stop (`proc_job_stop_self` + the `susp_stop_armed` freshness guard) landed
2026-08-13/14, days before this dossier's first write, and was never captured —
the Contract listed six entry points where there were seven, and Data-structures
read "three fields" for four. The dossier's own third-stop-source caveat
anticipated it abstractly while the source already existed. Added the entry
point, the field, a Mechanism subsection, and rewrote the caveat. A coverage gap,
not drift — but the currency bar is the same.

One code-comment drift was surfaced and left for its owner: `proc.h` still reads
"struct Proc stays 352" beside `debug_exitkill`, while the live
`_Static_assert(sizeof(struct Proc) == 392)` is correct — VIVARIUM's phenotype
fields grew the struct after that comment was written. The compile-time assert is
sound; only the prose comment drifted, so this is a [[sub-kernel-vivarium]]-arc
code fix, not a vault edit.
