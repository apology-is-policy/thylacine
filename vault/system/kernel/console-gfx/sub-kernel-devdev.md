---
id: sub-kernel-devdev
type: sub
parent: moc-kernel-console-gfx
title: "/dev — the namespace front door, and where the console's gates live"
code:
  - kernel/devdev.c
audit: hard
guarded-by: [inv-i27]
validated-by: [prose, gate-interactive, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 9.4"
  - "docs/IDENTITY-DESIGN.md section 9.8"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

The aggregating `/dev` directory: one Dev serving ten leaves and three
mount-point stubs, so that `/dev` is a walkable path rather than a set of
separate mounts.

Most of it is the boring Unix furniture — the bit bucket, the zero source, the
full disk, the two randomness aliases. Its **substance** is that it is the
console's second front door, and therefore where the trusted-path gates live.

## Contract

Single-level. Walk `/dev/<name>`, open, read or write. The three stub
directories exist so the resolver can traverse onto them and cross to the Dev
actually mounted there.

The walk **reuses the caller's pre-cloned Spoor** rather than minting its own —
the contract the resolver requires. A Dev that mints its own is unreachable
through the resolver, which is how the introspection Devs sat unmountable for
an arc ([[sub-kernel-devproc]]).

## Mechanism

### The gate is two-tier, and the tiers cover different things

The console leaves are gated at **open** — only a console-attached caller can
mint a console fd by name. That is the mint gate, and it is identical to the
syscall door's, which is the whole point: adding a walkable path adds no
ungated door onto a single-reader resource. An ungated open would let any Proc
become that reader and take a login passphrase out of the trusted chain's read.

The console **data** leaf is additionally re-gated on every read, write and
poll. The reason is specific: a path-only open skips the Dev's open entirely,
and the read syscall gates only on the handle's rights — so a gate-at-open-only
design would let a path-only handle read the console. A later walk-only flag
now also rejects such a handle at the syscall layer, so the re-gate is
belt-and-suspenders on the highest-stakes leaf rather than the sole defence.

The poll gate exists for a subtler reason than the read gate: an ungated poller
would register a wake hook on the console and **learn its input timing** without
ever reading a byte.

### The control leaf is deliberately not re-gated, and the fd is the capability

The line discipline must be settable by a Proc that is *not* console-attached —
the login program and the session shell set cooked and echo modes through an
**inherited** control fd, opened by the console-attached boot authority before
it relinquishes and passed down.

That is sound because of a conjunction, and all four terms are load-bearing:
the open-mint gate means only an attached Proc ever creates such an fd; the
walk-only flag blocks the path-open bypass; the fd reaches a non-attached Proc
only by deliberate spawn inheritance from the trusted chain; and the control
leaf **cannot read console input** — its read renders the mode line, so an
ungated write can flip the global termios but can never exfiltrate a keystroke.

The inherited fd *is* the capability. This is the one place in the console
surface where authority is carried by possession rather than by a check.

### The renderer pair is gated at open and re-gated everywhere

The output drain and the input feed are gated on the renderer role at open and
again at every read, write and poll — the same path-open rationale as the
console data leaf, applied to the two highest-stakes leaves in the file: the
drain carries **all** console output, and the feed injects into cooking, echo
and signal generation.

The drain's open also **arms** the tap, and unwinds the arm if the generic open
then fails — so a failed mint never leaves an armed drain with no fd behind it.
Its close disarms, gated on the opened flag: close fires for every clunked
Spoor including never-opened walk intermediates and path-only handles, and only
the one that actually minted carries that flag. Because close runs at the last
handle reference, a renderer that dies with the fd still open disarms through
the close-at-exit path.

### The renderer may mint a control fd, but only for one verb

The compositor is the authority on the display's geometry, so it self-serves a
control fd by name to report the window size — and it is not console-attached,
so it would fail the console gate.

The exception is admitted with an explicit **dominance** argument: the renderer
already holds the feed, which is arbitrary input injection, and that strictly
dominates reporting a geometry.

But dominance has a limit, and the limit is why the exception is *tagged*
rather than open: input injection does **not** dominate a termios flip, because
the termios word is global and also governs the serial receive path. So the
renderer-minted control fd carries a flag restricting it to the geometry verb;
a mode token on it rejects the whole write. Without that, a compromised
renderer could clear echo — or set it — on a serial login prompt.

The console **data** leaf stays attach-only for the renderer too. Reading
console input is exactly what the renderer role must not confer; the drain
carries output.

### The trivial leaves

Ungated, world-readable, the same on every Unix. Their presence is what makes
the gate visibly *leaf-specific*: the test that opens them from a non-attached
thread proves the gate is not a blanket one.

The full disk is the only one that refuses writes; the randomness leaves
consume writes rather than stirring the pool, which is a deliberate deferral
since the generator reseeds on its own cadence.

## Data structures

A static leaf name table and an enumeration of kinds. The qid path **is** the
kind, so resolution is a table scan and every dispatch is a switch on the qid.
No allocation, no per-open state — except the restriction flag on a
renderer-minted control fd, which lives on the Spoor.

## Concurrency

None owned. Every gate is a query into the process module, which takes
[[lock-proc-table]] itself; the console operations take the console's locks
([[sub-kernel-cons]]). The leaf table is static and const.

## Invariants enforced

**[[inv-i27]]** — this file is one of the invariant's two enforcement sites.
It owns the "every door gates identically" clause and the whole
renderer-versus-attached separation.

Composes **[[inv-i1]]**: reachability is namespace visibility. A Proc without
`/dev` in its namespace reaches none of this.

## Error paths

`NULL` from open for a gate denial — which the walk-open syscall surfaces as
`-1`. `-1` from read or write for a denial, for a leaf that does not serve that
direction, and for an unknown kind. `POLLNVAL` for a poll denial.

`-1` from stat for an unknown kind — deliberately, rather than inventing a
shape for a qid the switch does not recognize.

## Performance

Nothing of its own. Walk is a scan of a ten-entry static table; every dispatch
is a switch on an integer qid; nothing allocates. The cost of a console
operation is the console's ([[sub-kernel-cons]]), and the cost of a gate is one
process-table lock acquisition per gated call — which is why the console data
leaf's per-I/O re-gate is a real (if small) tax that the open-only gates avoid.

The zero and full leaves fill the caller's buffer a byte at a time rather than
by word or by a page-sized memset; irrelevant at their current use, and the
first thing to look at if anything ever streams from them.

## Prosecution

- **A new console-adjacent leaf must be added to the right gate set.** The sets
  are three separate predicates (console, console-data, renderer) and a leaf in
  none of them is ungated by default.
- **The renderer-minted control fd must stay restricted to the geometry verb.**
  The dominance argument covers geometry and not the global termios.
- **The console data leaf must stay attach-only**, for the renderer as much as
  for anyone.
- **The drain arm must stay paired with its unwind**, and the disarm must stay
  gated on the opened flag — walk intermediates and path-only handles reach
  close too.
- **The walk must keep reusing the caller's Spoor.** A Dev that mints its own is
  unreachable through the resolver.
- **A new leaf needs four registrations** — the kind, the name table, the read
  and write dispatches — plus the stat switch, and see below for which of them
  fails quietly.

## Seams

- **Readdir.** `/dev` cannot be listed; the root read refuses, matching the
  sibling introspection Dev. A shell glob over `/dev` finds nothing.
- **The control leaf's poll is a constant.** It reports always-ready and
  installs no hook, which is honest for a control surface with no readiness —
  but means a poller cannot wait on a mode change.

## Caveats

- **The geometry leaf has no stat, and that is a live gap rather than a
  design.** The stat switch enumerates every other kind and the geometry leaf
  falls to the default arm, so `fstat` on `/dev/winsize` returns `-1`. Three
  things make this worth recording rather than shrugging at. The file's own
  vtable comment states the intent as *every* leaf answering. The rationale
  written beside the switch is that a stat failure with the wrong errno is
  fatal to a real toolchain — the compiler treats it that way on its standard
  fds, which is the defect that put the slot there in the first place. And the
  neighbouring stat test's header says it covers "every leaf shape" while
  enumerating a hardcoded list of five that predates this leaf. Meanwhile the
  geometry leaf's *own* test records the absence as though it were intentional
  ("the leaf itself is statless"), so the tree currently states the rule and its
  exception in two comments that cannot both be right. The consequence is
  bounded — no current consumer stats it, and this leaf is *newer* than the
  compiler fix — but the leaf exists precisely so that ordinary unprivileged
  programs can read the geometry without minting a control fd, which is exactly
  the population that stats what it opens. Tracked as
  [[seam-devdev-winsize-statless]].
- **The stat switch is the registration that fails quietly.** A leaf missing
  from the read or write dispatch is dead on arrival and obvious; a leaf missing
  from the stat switch walks, opens, reads and writes perfectly and fails only
  when someone stats it. That is how this one got in, and it is the same shape
  as the introspection Dev's silently-failing read whitelist
  ([[sub-kernel-devproc]]).
- **The mode bits are documentation.** The Dev does not set the permission-
  enforcement flag, so the reported modes — a console at `0620`, the renderer
  pair at `0600`, the trivial leaves at `0666` — describe the gates rather than
  implementing them. Same posture as the introspection Devs, same operational
  consequence: grep the gate, not the mode.
- **The control leaf's read is offset-aware over a freshly rendered line**, so a
  paginated read re-renders. Harmless at 54 bytes, but see the mode-render
  buffer-contract drift in [[sub-kernel-cons]].

## Provenance

[[chg-2026-08-02-console-sweep]].
