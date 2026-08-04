---
id: sub-prowl
type: sub
title: "prowl — a process monitor that cannot know what it is not being told"
parent: moc-userspace-tools
code:
  - usr/prowl/src/main.rs
  - usr/prowl/src/sample.rs
  - usr/prowl/src/ui.rs
  - usr/prowl/Cargo.toml
audit: none
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/PROWL-DESIGN.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The scheduler-aware process monitor: a full-screen console application
that polls the kernel's process and CPU telemetry, derives per-process CPU
percentage the way `htop` does, and lets the user kill, suspend or resume
the selected process.

It is the only consumer in the tree of the process and control filesystems
as a *live* surface rather than a one-shot query, which makes it the
place where those surfaces' limits become visible to a person.

## Contract

Poll the process table and the per-CPU counters roughly every one and a
half seconds; diff the cumulative nanosecond counters against the previous
poll and divide by the measured wall interval. One hundred percent means
one core fully busy, so a process across two cores reads two hundred.

Keys navigate, sort, toggle a tree view and a per-thread detail pane, and
apply three control verbs. Kill is confirm-gated; suspend and resume are
not, on the stated ground that both are reversible.

**It adds no authority.** Every control verb is a write to the target's
control file, and the kernel's two-axis check — owner, or the relevant
capability — decides. A confined user acts only on processes it could
already act on. The monitor confers nothing and validates nothing on the
kernel's behalf.

## Mechanism

**Three layers, split by what each may touch.** The sampler is pure: it
parses telemetry text into rows and derives rates, and it is deliberately
terminal-free *and* clock-free — the caller supplies the elapsed interval.
The UI draws into a back buffer. The main module owns the console, the
event loop, and the control writes.

**The percentage math is integer-only**, in tenths of a percent, because
there is no float. The delta cannot exceed the interval times the core
count, so the multiply stays well inside range.

**The CPU meter inverts an idle counter.** Utilization is one minus the
idle delta over the wall delta, clamped, so a core that parked the whole
interval reads zero and a fully busy one reads a hundred. The clamp
protects against clock-domain skew between the two measurements.

**The cursor tracks a process identifier, not a row index**, so it stays
on the same process across re-sorts and list churn. Navigation steps the
*display* order rather than the underlying vector — which matters in tree
mode, where the render is a permutation, and was a real fix: stepping the
flat order made the cursor skip rows the user could see.

**The tree walk is cycle-safe and orphan-safe.** A visited set bounds it
to one visit per row, roots are any row whose parent is not in the set,
and anything unreached — a cycle, a stale parent, a parent truncated away
— is appended rather than dropped. The view never loses a process it was
given.

**A denied detail read and a vanished process are deliberately
conflated.** The per-thread pane's source returns empty for both, so the
pane says "unavailable" either way. That is honest: the caller genuinely
cannot distinguish them, and pretending otherwise would require a
distinction the kernel does not offer.

## Data structures

A row per process (identifiers, name, state, thread and page counts,
cumulative and derived CPU), a row per CPU, and an optional parsed
per-thread detail block. Two samplers each hold the previous poll's
counters as a small association list — linear scans, which at this scale
are the right choice.

## Concurrency

None. Single-threaded, one poll loop, keys and the refresh interval
sharing one wait.

## Invariants enforced

None. It reads what it is shown and writes verbs the kernel adjudicates.
Its console posture is the abstention the shell-side contract requires: it
owns the screen on stdout and reads keys on stdin, and never touches the
line discipline. The shell puts the console into raw mode around it and
restores it afterwards — including after a crash, since a native binary
aborts on panic and its cleanup does not run.

So prowl is never console-attached and the trusted-path invariant is
untouched. A buggy prowl corrupts its own screen.

## Error paths

An unreadable telemetry file yields an empty string, and prowl degrades to
an empty list rather than failing. A denied control write returns false
and becomes a status line saying "denied".

The degrade is the right shape for a monitor — better a blank list than a
crash — with the consequence noted below.

## Performance

One or two small reads per tick plus one per open detail pane. The tree
build is quadratic in the row count via a linear parent lookup, which at
the sizes reachable here is nothing.

## Prosecution

- **The sampler must stay pure.** Its whole value is that it is
  terminal-free and clock-free; a direct clock read inside it would make
  the rate derivation untestable and unreproducible.
- **Control writes must stay unadorned.** The kernel decides; prowl must
  never pre-filter which processes it offers a verb for, because that
  would encode an authority model beside the real one.
- **Navigation must keep stepping the display order.** The flat-order bug
  is fixed and would return the moment a new view mode is added without
  routing through the display-order helper.

## Seams

The refresh interval is fixed. There is no filter or search. The detail
pane covers one process at a time.

## Caveats

- **It reports a truncated process list as the complete one, and it cannot
  do otherwise.** The kernel's process-table renderer stops walking when
  its fixed buffer fills, at roughly thirty processes. prowl's header row
  prints the count of rows it parsed — presented, reasonably, as the
  number of processes.

  prowl's own comment attributes this correctly to a kernel pagination
  seam and not to itself. The attribution is right and the gap survives
  it: the kernel *computes* the truncation into a field named `overflow`,
  sets it at fifteen distinct points, and then returns the byte count and
  discards it. Fifteen writes, zero reads. So from the client side a
  truncated read is byte-indistinguishable from a complete one — no
  marker, no count, no short read — and no amount of care in prowl can
  recover the fact.

  The workload that reaches thirty processes is a parallel build, which is
  exactly when a person opens a process monitor. So it under-reports
  precisely under the load it exists to observe (task #158).

- **The staging buffer's stated headroom does not exist.** prowl reads
  into four kilobytes, noting that the kernel caps at two. Harmless, but
  the comment implies a margin that is unreachable.

- **The pure layer is built for testing and has no tests.** The sampler is
  terminal-free, clock-free, and takes its elapsed interval as a
  parameter — every structural choice a testable module makes. Its
  manifest then says outright that prowl is "a device-only native TUI (no
  host-tested lib half like nora)", so nothing runs.

  What goes untested is the arithmetic that is the whole product: the
  cross-poll rate derivation, the counter-went-backwards case on
  identifier reuse, the idle-inversion clamp, the eight-column parse and
  its deliberate rejection of a nine-token line, and the cycle-safe tree
  walk. Each is a pure function over a string and an integer. The
  refactor that would run them is one manifest line and a feature gate —
  the pattern two sibling crates already carry.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
