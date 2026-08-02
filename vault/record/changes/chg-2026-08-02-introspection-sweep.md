---
id: chg-2026-08-02-introspection-sweep
type: chg
title: "vault sweep: the kernel introspection Devs"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-devproc
  - sub-kernel-devctl
established:
  - inv-i26
  - inv-i39
  - inv-i22
  - spec-debug-stop
  - spec-pty-stop
  - lock-vma
  - lock-env
closed: []
opened:
  - seam-proc-name-torn-read
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 14, the batch batch 13 said it needed. Read from code: `devproc.c` (2669
lines — the largest single file in the kernel tree) and `devctl.c`, plus the
stop/resume and job-control primitives they drive in `proc.c` and the Spoor
close-hook ordering in `spoor.c`. Two dossiers under
`system/kernel/introspection/`.

WHY THIS BATCH, AND NOT THE REGISTRY PASS. Batch 13 set out to mint the
invariant family and was stopped by the schema's own field list: an `inv` note
requires `guards` — sub ids — and four of the nine authority invariants had no
swept enforcement home. Two of those four, **I-26** and **I-39**, are enforced
in `devproc.c` and nowhere else. This batch is the dependency, pulled forward.
After it, only I-27 (the trusted path, in `cons.c`) is still homeless.

THE ORGANIZING FACT is that these Devs widen VISIBILITY, never AUTHORITY — and
the mechanism is that **neither enforces its own mode bits**. Both set
`perm_enforced` false, so every mode `stat_native` reports is documentation and
every real gate is a check at the read or write site. devproc records why it had
to be built that way rather than at open: the shared open chokepoint hard-rejects
before `devproc.open` runs, so the `CAP_KILL` axis could not live there. The
operational consequence is worth carrying: **a mode bit on this surface is a
comment; grep the read site.**

FIVE GATES, AND THE NEAR-MISSES ARE THE DESIGN. kill (owner or host-owner or
kill-anyone), owner-or-host-owner (the telemetry files), debug (kproc- and
notrace-refused first, then owner or host-owner or debug-anyone), slot ownership
(stricter than the debug gate — only the attached debugger drives run state), and
the kernel-base gate, which is capability-ONLY because the kernel has no
owner-principal. `CAP_DAC_OVERRIDE` is on none of them: fs-admin stays orthogonal
to process control, stated twice in the code at both gates that could plausibly
have taken it. Reading a process's scheduler internals is not killing it, so the
telemetry gate is strictly narrower than the kill gate beside it; reading them is
not debugging either, so the debug capability is absent from both.

DEVPROC NEVER HOLDS A PROC POINTER. Sixteen `proc_for_each` sites; every
operation is resolve-by-pid under the global lock, authorize, act, return a
status code. The blocking waits re-resolve on every poll round rather than
caching, so a target reaped mid-wait is simply not found instead of a
use-after-free. That single discipline is the lifetime argument for the whole
file.

THE ATTACH SLOT IS A BARE POINTER, AND ITS SOUNDNESS IS EXTERNAL. `debug_owner`
holds a ctl Spoor as an identity token, never dereferenced, compared only by
equality. It cannot dangle because `spoor_clunk` runs the Dev close hook exactly
once on the last ref drop, BEFORE freeing the storage — so the slot is always
cleared before the pointer it names can be reallocated. Verified in `spoor.c`
rather than assumed. The hook additionally must run OUTSIDE `g_proc_table_lock`
(it takes it via `proc_for_each`); both current callers have dropped it.

FULLY-STOPPED IS A CONJUNCTION AND EACH CONJUNCT CLOSED A REAL FAILURE — the
off-CPU spin (a thread mid-context-switch still reads on-CPU while its frame is
being written), the parked check under each peer's own wait lock (the same lock
the park's register-then-observe takes, so it can never confirm a thread about to
proceed to EL0), and the group-exit check (a dying target's threads go EXITING,
which the parked scan skips, then write their context outside the global lock).
Death wins over a stop, everywhere.

THE READ DISPATCH IS A STRICT PARTITION, CENSUSED. Two mutually exclusive
machineries serve reads — format-and-slice into a 2 KiB buffer (8 kinds) and a
purpose-built path (7 kinds). A census of the four registration points found the
partition intact: all fifteen kinds in the name table and the mode table, and in
exactly one of {format dispatch + read whitelist} or {own path}, none in both,
none in neither. Clean — which is the honest result, and it establishes the
maintenance obligation: **adding a file means four registrations, and the read
whitelist is the one that fails SILENTLY** (the file resolves fine and reads -1
forever; the `maps` chunk shipped with exactly that omission).

NEW SEAM: [[seam-proc-name-torn-read]] (task #17). `proc_set_name` stamps
`Proc.name` at exec byte-by-byte, terminator last, WITHOUT `g_proc_table_lock` —
and its comment argues the race away: "Runs in the CHILD's context... no
concurrent reader observes a torn stamp." True of the execing Proc's own threads;
false of the three cross-Proc readers this very sweep is about (`/ctl/procs`,
`/proc/<pid>/status`, `/proc/<pid>/sched`), which read `p->name` from another CPU
under a lock the writer does not take. The monitor tool polls the process list
continuously, so a concurrent exec is the ordinary case.

It is safe today — but NOT for the reason written down. It is safe because of an
unstated bound in the writer's loop: the copy continues only while the index is
strictly below `PROC_NAME_MAX - 1`, so the array's final byte is only ever
written as the terminator and is otherwise zero from the allocator. The array
therefore always terminates within itself, at any instant. That matters because
the reader walks until it finds a zero and is bounded by its OUTPUT buffer, not
by the source array: without that property a mid-stamp read could walk out of
`name[]` into adjacent `Proc` fields and copy them into a file userspace reads.
The bound IS pinned by a test — framed as string hygiene ("long name stays
NUL-terminated"), not as the bound the readers depend on, so nothing connects the
two. Effect today is a cosmetic mixed old/new name for the length of one stamp;
the defect is that a real safety property is held by an accident of a loop bound
while an inaccurate justification sits at the writer — in functions that
otherwise read EVERY per-thread field with an explicit relaxed atomic load,
carrying comments about exactly this hazard. The discipline stopped one field
short.

TWO STALE CITATIONS, in the batch-13 direction — prose drifting from code the
tests kept honest.

The `exe` clamp justifies itself by describing an out-of-bounds read "at offset
>= 512" against a 512-byte buffer. The buffer was raised to 2 KiB and the maximum
path is 1 KiB, so the described scenario can no longer occur and the clamp is
currently inert. The clamp is still correct defence; it is the arithmetic in the
comment that no longer holds, and a reader who checks it concludes the guard is
unnecessary.

The focus-thread selector says it is non-static "so the selection is
unit-testable" and names a test case. **No case is registered under that name.**
The first search for the cited name found nothing and looked like a coverage gap
discharged only on paper — the follow-up search for the SYMBOL found four real
assertions, including the foreign-focus fallback the comment insists must never
be deleted, living inside the stop/start/resume test. So the coverage is real and
load-bearing and only the citation is wrong. Recorded because the correction is
the lesson: **grep the symbol, not the test name a comment cites.**

DEVCTL IS DEFAULT-ALLOW, AND THE PROJECT HAS ALREADY CHOSEN DEFAULT-DENY
ELSEWHERE. One leaf is gated (`kernel-base`, the KASLR slide); everything else is
world-readable, so a new leaf is readable by everyone unless someone remembers a
line. The compositor's control surface was restructured after an audit to deny
everything except an explicitly enumerated ungated set, precisely so a newly
added verb is gated by construction. The two surfaces genuinely differ — reads
under a deliberate Plan 9 all-visible posture here, authority writes there — so
default-allow is defensible rather than wrong. But the asymmetry is a decision
and deserves to be written down as one, because a leaf carrying a secret is one
forgotten line from disclosure.

TWO DEFENSIVE SHAPES WORTH CARRYING. A never-online CPU has an idle time of zero
forever, which through the meter's own arithmetic is INDISTINGUISHABLE from a
permanently pegged core — so the renderer emits a short `offline` row the reader
skips rather than a number that would draw a dead core as a full meter. And every
row-formatting loop commits a row only once it wholly fits, which is not
cosmetic: it bounds the walk by the OUTPUT BUFFER rather than by the number of
VMAs or threads, so a process at the VMA maximum holds its lock for tens of rows
rather than tens of thousands.

THE REGISTRY PASS IS NOT ONE PASS — IT IS THE SWEEP'S TAIL. Batch 13 read the
schema's step 3 / step 4 ordering as "sweep everything, then mint the registries",
and concluded the registry work was blocked. Half right. The linter settles it by
construction: frontmatter id references are checked for EXISTENCE, not just shape,
so a dossier citing a spec or a lock cannot be committed until that note exists —
which means every batch has been minting registry notes all along (seventeen
specs, twenty locks, fourteen invariants standing before this one). What batch 13
actually hit was narrower than it looked: not "the registries are blocked" but
"these four invariants have no `guards` home yet."

So this batch mints its own tail, and closes the loop it opened: **[[inv-i26]] and
[[inv-i39]]** (two of the four batch 13 could not write), plus
[[spec-debug-stop]], [[spec-pty-stop]], [[lock-vma]] and [[lock-env]]. Of the
authority family, only I-27 is still homeless — the trusted path, in `cons.c`,
which is the next sweep.

**[[inv-i22]] came along for a reason worth recording.** Batch 13 wanted to link
it and could not, so it demoted the link to plain text. Writing [[inv-i26]] hit
the same dangling link — and this time the answer was not to demote again: I-22's
enforcement home (`perm_check`) had been swept by batch 13 itself, so the note was
already mintable and nobody had noticed. It is a good invariant to have minted
early, because it is enforced almost entirely by ABSENCES — no system-principal
branch in the permission check, no elevation-only capability surviving a fork, no
identity short-circuit in the process-control gates — and an absence is precisely
what a future "helpful special case" restores without any test going red. The
batch-13 plain-text demotions are upgraded back to links.

Which leaves the honest scope line: I-2, I-5, I-6, I-23, I-25 and I-34 are ALL now
mintable — their homes were swept in batch 13 — and are deliberately left to the
registry pass rather than cascading further here. A sweep that mints its own
dependencies is finishing its work; a sweep that mints everything reachable is a
different chunk wearing this one's name.

A THIRD STALE COUNT, found while writing the spec note. Scripture describes
`debug_stop.tla` as "clean + 6 buggy cfgs"; the tree carries **seven**
(`park_before_die`, `lost_stop`, `double_wake`, `strand_on_debugger_death`,
`fault_stop_ungated`, `stop_skips_sleeper`, `exitkill_ignored`). The count was
right when the exitkill work recorded it as "now 6" — counting the six that
preceded its own addition — and was not re-derived when that addition landed. The
spec note carries the as-built eight cfg lines.

THE `guards` CHECK IS REFERENTIAL, NOT TYPE-AWARE — probed, not assumed. Batch 13
verified its layout blind spot rather than asserting it; this batch probed the
mechanism that stopped it. Two probes on a live invariant note:

- pointing `guards` at a **nonexistent** id fails on exactly that
  (`guards -> unknown id`), and additionally staled the generated invariant view
  — so the edge is load-bearing in both directions;
- pointing `guards` at a note that **exists but is the wrong type** — an
  invariant declaring a TLA+ spec as its enforcement home — lints **CLEAN**,
  707 notes, 0 fail, 0 warn.

So batch 13's blocker was real but **self-imposed**: the linter would have
accepted any existing id, and the discipline that made it stop was judgement, not
tooling. That is the right call and worth having made — but it means a future
batch under pressure can satisfy the gate with a meaningless edge and leave the
corpus lint-green while carrying an invariant whose "enforcement home" is a spec
file.

Taken with batch 13's layout probe, the shape of the tool is now measured rather
than guessed: **the linter enforces referential integrity and required-field
presence; it does not enforce meaning or placement.** Both blind spots are
human-covered by construction, so both have to be read by hand, every batch.

LAYOUT: `introspection/` was declared-and-empty since commit 0, so the dossiers
went there with no schema change — batch 13's amendment plus reading the layout
prospectively meant this batch needed neither a new directory nor a correction.
