---
id: sub-mechanism-drivers
type: sub
title: "ptyhost and loom-stress — two one-file programs that exist to drive a kernel mechanism"
parent: moc-userspace-tools
code:
  - usr/ptyhost/src/main.rs
  - usr/loom-stress/src/main.rs
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/PTY-DESIGN.md", "docs/LOOM.md"]
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

Two single-file programs whose reason to exist is the same: each is the
**first real userspace consumer** of a kernel mechanism that until then
had only been reasoned about.

The session host mints a pseudoterminal, runs a program on the slave side,
and pumps bytes between the outer terminal and the master — which is
exactly what one window of a terminal multiplexer does, with one window
and no interface. The stress program drives the asynchronous ring API from
several real threads under multiple processors.

They are grouped because that shared purpose is the interesting thing
about both, and because it is a distinct answer to "how is this proved":
not a test, not a self-test, but *a program that does the thing for real,
run every boot*.

## Contract

**The session host** takes a program (defaulting to the shell) and hosts
it on a fresh pseudoterminal, relaying between its own standard input and
output — the console when launched from a console shell, or an outer
pseudoterminal when nested — and the master side.

**The stress program** runs after the root pivot, so the disk filesystem
is live and the ring's payload operations actually dispatch. Three phases:
a positive round-trip that writes, syncs and reads back over the ring; a
concurrent phase where two sibling threads share one ring; and a
cross-process-death phase.

## Mechanism

**The session host is trivial because of a composition it does not
perform.** It is registered in the shell's raw-command set, so the *outer*
shell flips the outer console raw around it and restores it afterwards.
The outer terminal collapses to a byte pipe: interrupt and suspend
characters arrive as raw bytes, are pumped inward, and the pseudoterminal
is the one line discipline that cooks them into the inner session's
signals.

So the host installs no discipline of its own and never touches the
console control device. The inner shell's own session setup does the rest.
That is the whole trick, and it is why the file is short: the mechanism
was designed so that the first consumer needs no special case.

**The stress program is the harness five audit closes owed.** The
asynchronous ring's concurrent paths — the elected reader, the per-ring
borrow guard, the completion wait-list, and quiesce-on-process-death —
were each reasoned about in review and could not be exercised
deterministically, because the synchronous test harness cannot produce the
interleavings. This program produces them from real threads under real
scheduling, and runs under the multi-processor boot matrix.

Each thread serializes its own access to the submission and completion
queues under a lock while entering the ring concurrently with its sibling,
which is the shape the ring's single-driver contract requires and the
shape a real consumer would use.

## Data structures

Almost none. The host holds two staging buffers and the descriptors; the
stress program holds a shared ring and per-thread state.

## Concurrency

The stress program is one of very few native programs in the tree that is
genuinely multi-threaded, and that is its point. The session host is
single-threaded with a poll loop.

## Invariants enforced

Neither enforces one; both *exercise* several. The host composes with the
trusted-path split by abstention — it never touches the line discipline,
so the console's capability story is unchanged, and the discipline that
cooks its bytes is the pseudoterminal's, which is where the terminal
invariant's data path lives.

The stress program's whole value is as evidence for invariants the kernel
enforces: no lost completion, no stale completion, the submit-time
capability pin, and clean teardown when a process dies mid-flight.

## Error paths

Both fail loudly and early. The stress program's phases assert, so a
regression surfaces as a boot-visible failure under the multi-processor
matrix rather than as a quiet wrong answer.

## Performance

Not a goal for either. The stress program's cost is deliberate: it exists
to create contention.

## Prosecution

- **The host must keep not touching the line discipline.** Its entire
  safety argument is that the outer shell owns the outer console's mode
  and the pseudoterminal owns the inner one. A host that set a mode itself
  would have two owners for one piece of state.
- **The stress program's concurrency must stay real.** Serializing the two
  threads through one lock end-to-end would make it pass while testing
  nothing — the interleaving *is* the coverage.
- **It must keep running after the pivot.** Before it, the ring's payload
  operations have no live filesystem to dispatch against, so the phases
  would pass vacuously.

## Seams

The host is one window with no interface; the multiplexer it is the core
of is unbuilt. A thematic rename is held.

The stress program's third phase is timing-dependent by nature — it wants
operations in flight at the moment a process dies — and is documented as
best-effort with a measured margin rather than as a guarantee.

## Caveats

- **The stress program describes itself as a harness and is counted as
  production code.** Its own opening line calls it "the concurrent +
  cross-Proc-death SMP stress harness", and the coverage census — which
  excludes probes, smokes, benches and torture programs as harness —
  counts it among the owned-or-unowned production files. Not wrong in any
  consequential way, but it means the census's harness exclusion is
  name-shaped rather than purpose-shaped, and this file is the visible
  edge of that.

- **Neither has a test, and for once that is the right answer.** A test of
  the session host would have to build a pseudoterminal and a child and a
  relay, which is what the host *is*. A test of the stress program would
  be a test of a test. Their proof is that they run, unattended, on every
  boot of the relevant configuration — and in the stress program's case
  across the full multi-processor matrix, which is stronger evidence than
  any unit test of the same code could give.

  Worth stating plainly because the rest of this area's dossiers record
  missing tests as a gap. Here the absence is a correct judgement, and the
  distinction is whether the program's own execution *is* the assertion.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
