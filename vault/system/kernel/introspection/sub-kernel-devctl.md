---
id: sub-kernel-devctl
type: sub
parent: moc-kernel-introspection
title: "/ctl — machine-wide stats, and the one gated leaf"
code:
  - kernel/devctl.c
audit: light
guarded-by: []
validated-by: [prose, gate-smp]
locks: [lock-proc-table]
abis: []
design: ["docs/ARCHITECTURE.md section 9.4", "docs/PROWL-DESIGN.md section 3.4", "docs/VIVARIUM.md section 6.17"]
created: 2026-08-02
updated: 2026-08-16
---
## Purpose

The `/ctl` Dev (`dc='C'`, uppercase to leave `c` for the console): eight flat
text files rendering machine-wide state — the process list, physical memory,
the registered Devs, the KASLR base, scheduler stats, per-CPU meters, the
console's admission counters, and the live 9P sessions.

Read-only. Admin *commands* were deferred and never landed; `write` refuses
unconditionally.

## Contract

Walk `/ctl/<leaf>`, read text. Single-level: `..` from anywhere is the apex, and
a walk from a leaf has no meaning. Offset-aware reads over a freshly generated
2 KiB snapshot.

`stat_native` reports the apex as a directory and each leaf as a regular file,
**with size 0** — deliberately. Every file is generated at read time from live
state, so any length measured at stat is already stale, and a caller that
trusted it (stat, allocate, read exactly that many bytes) would silently truncate
a growing table. Linux reports 0 for `/proc/meminfo` for the same reason and the
world's readers loop to EOF. Sibling Devs that report *real* sizes are correct to
do so: their content is a static device-tree property or a config register, which
does not move between the stat and the read.

## Mechanism

### One table drives everything

A single leaf table carries `{name, kind, formatter}`, and walk, stat and read
all resolve through it. That is structurally better than the sibling `/proc`,
where adding a file means four separate registrations — here there is one, and a
leaf that is in the table is automatically walkable, stattable and readable.

### The gate is a special case, and it is default-allow

Exactly one leaf is gated: `kernel-base`, which discloses the live KASLR slide.
The gate is a `CAP_HOSTOWNER` check with **no owner axis** — the kernel has no
owner-principal, so the capability axis is the only one that could exist. A
logged-in user is stripped of the elevation-only capabilities at fork, so it
cannot read the slide and defeat the mitigation.

Everything else is world-readable Plan 9 introspection: the full process list
with names, parents, states, thread counts, page counts and CPU time, visible to
any Proc that can name `/ctl`. That is the deliberate posture, not an oversight —
but see Caveats for the shape it leaves behind.

The mode reported by `stat_native` follows the gate (0400 for `kernel-base`,
0444 elsewhere) so the advertised mode does not lie about a file the caller
cannot in fact read — but as with `/proc`, `perm_enforced` is false, so that mode
is documentation and the check at the read site is the enforcement.

### Zero means overflow, and an empty string writes zero bytes

The emit macros are the file's whole formatting discipline: call a helper, and
**treat a return of zero as "the buffer is full"** — set the full flag, abandon
the row, return. Every field in every row goes through them.

An empty string writes zero bytes. So emitting one is indistinguishable from
running out of space, and the format aborts at that point.

This is not hypothetical: the newest leaf carried a conditional suffix written as
a ternary with an empty alternative, and every read of that file truncated at
**exactly** the same offset — deterministically, which is what ruled out
interleaving and pointed straight at the format rather than the console. One
partial row and nothing after it, on a file whose entire purpose was diagnosing
something else.

**A success that produces nothing is indistinguishable from a failure that
produces nothing.** The convention has no room to say "wrote zero bytes, on
purpose" — which is the same shape as a gauge reading zero because the thing
never started.

The repair is stated as a rule at the site and generalizes past the literal that
caused it: never route a possibly-empty value through an emit. Conditional
suffixes are guarded by an `if` instead of a ternary with an empty arm, and a
**runtime-computed** value that could be empty — a session label — emits a
placeholder rather than nothing. The surrounding ternaries had always had two
non-empty arms, which is why only the new code broke; the rule was being followed
before anyone had written it down.

**The test passed through all of it.** It asserted a *prefix* of the row, and the
prefix sat before the truncation point — so the assertion could not observe the
failure it was there to catch. It now asserts through the row's tail, which is
the only version a mid-row abort cannot satisfy.

### The process list bounds its own lock hold

The per-Proc formatting callback returns *stop* on the first overflow, so the
walk ends when the output buffer fills rather than visiting every process under
the global lock with interrupts off. Once `/ctl` became reachable from userspace
this stopped being a formatting nicety: it is what bounds an unprivileged
tight-loop reader's lock hold to the size of the buffer instead of the size of
the process table.

### Offline CPUs render as a short row, not as a busy one

A CPU declared by the device tree that never came online has an idle time of
zero forever — which through the meter's arithmetic (`1 - idle/wall`) is
**indistinguishable from a permanently pegged core**. So the per-CPU renderer
gates on the online flag and emits a two-token `offline` row that the reader's
three-token parse skips, rather than a number that would draw a dead core as a
full meter.

The row format is append-only by contract: the userspace reader matches the
first three tokens positionally and ignores the rest, so columns may be added.

## Data structures

None owned. The leaf table is static and const; all state is read live from the
process table, the physical allocator, the Dev registry, the KASLR module and the
per-CPU scheduler meters.

## Concurrency

Only `g_proc_table_lock`, and only for the process list (taken by
`proc_for_each`; the CPU-time helper walks a Proc's threads under it).

Everything else is read without a lock, and each has its own reason: the physical
counters and scheduler stats are coherent atomic snapshots of a single writer;
per-CPU capacity, cache-line size and MIDR are boot-static; the Dev registry is
boot-immutable. The job-stop flag surfaced in the state column is read atomically,
because a cross-Proc reader holds no per-Proc lock.

## Invariants enforced

**I-16** (the KASLR slide is a secret) — the `kernel-base` gate is one of its two
enforcement sites, the other being the `/proc` kernel-stack raw/symbolic split.

Composes **I-1**: reachability is namespace visibility.

## Error paths

`-1` for: a read of the apex directory, a qid this Dev does not serve, a denied
`kernel-base` read, and every write. No errno distinction, matching the sibling
Dev.

## Performance

One 2 KiB stack buffer, regenerated on every read — so a paginated read of a
large process list re-renders the whole list per call. Fine at the current scale;
the same offset-aware multi-read that `/proc` wants would fix both.

## Prosecution

- **A new leaf that discloses a secret must add its own gate.** The gate is a
  per-leaf special case in an otherwise world-readable file, so the default for a
  new leaf is *readable by everyone*.
- **The `kernel-base` gate must stay capability-only.** There is no owner to
  admit.
- **The process-list callback must keep stopping on overflow**, or an
  unprivileged reader re-acquires an unbounded global-lock hold.
- **Sizes must stay 0** for generated leaves. Reporting a measured length invites
  the stat-then-read truncation the current shape exists to prevent.
- **The offline-CPU row must stay short.** Emitting a numeric idle time for a
  never-online CPU renders a dead core as a pegged one.
- **Row columns are append-only.** Readers parse positionally.
- **Never emit a possibly-empty value.** Zero bytes written *is* the overflow
  sentinel, so an empty string aborts the whole file. Guard conditional suffixes
  with a branch rather than a ternary carrying an empty arm, and give any
  runtime-computed field a placeholder.
- **A row's test must assert through its tail.** A prefix assertion sits before
  wherever a mid-row abort would land, so it passes on exactly the failure it
  exists to catch.

## Seams

- **Writes.** The admin command surface (scrub, allocator controls, scheduler
  tunables) was deferred to the phase that would expose `/ctl` to operators and
  has not landed; `write` refuses.
- **Nested directories.** The architecture describes `/ctl/kernel/...`; the
  as-built layout is flat, pending a Dev that walks more than one level.
- **The whole-file re-render per read**, shared with `/proc`.

## Caveats

- **The read gate is default-ALLOW, and the project has already chosen
  default-DENY elsewhere.** The compositor's control surface was restructured so
  that its gate denies everything except an explicitly enumerated ungated set,
  precisely so a newly added verb is gated by construction. `/ctl` has the
  opposite shape: a new leaf is world-readable unless someone remembers a line.
  That is defensible for a surface whose *posture* is Plan 9 all-visible
  introspection, and the two surfaces differ (reads here, authority writes
  there) — but the asymmetry is a decision, and a leaf carrying a secret is one
  forgotten line from disclosure.
- **The formatters ignore a failed numeric append.** Several leaves add a
  number without checking whether it fit, then check the following literal. Each
  append is independently bounds-checked so there is no overflow; the visible
  effect of an exhausted buffer is a line missing its value rather than a
  truncated file. The sibling Dev checks both.

  **This caveat named the right convention and only one of its two directions.**
  It warned about a genuine failure being *ignored*. The defect that actually
  landed was the mirror image — a genuine success being *read as failure*,
  because zero is the overflow sentinel and an empty string writes zero bytes.
  Same fragile convention, opposite direction, and worse in effect: the ignored
  failure loses a field, the invented failure loses the rest of the file. Having
  enumerated one direction made the write-up read as though the hazard had been
  covered.
- **The default-allow read gate has now been exercised, and the default was
  right.** A new leaf landed and is world-readable, which is exactly the shape
  the caveat below predicts. Its content — peer identifiers, buffer counters,
  frame counts — is ordinary introspection and narrower than the process list
  already beside it, so nothing was disclosed that should not have been. Worth
  recording as evidence about the *rate* rather than as a refutation: the
  mechanism fired once and landed benignly, which is what a default-allow shape
  does until the one time it does not.
- **The process list is a full-system disclosure.** Names, parents, states,
  thread and page counts and CPU time for every process, to any reader. This is
  the Plan 9 posture and is shared with `/proc/<pid>/status`, but it is worth
  stating plainly rather than leaving implied: `/ctl/procs` is the broadest
  ambient disclosure either introspection Dev makes.
- **The formatting helpers are duplicated** from the sibling Dev, noted in the
  code as deliberate chunk independence. They have since drifted slightly (the
  hex helper differs in prefix handling), so a fix to one does not reach the
  other.

## Provenance

[[chg-2026-08-02-introspection-sweep]], [[chg-2026-08-16-devctl-empty-emit]].
