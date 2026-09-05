---
id: sub-coreutils-lib
type: sub
title: "coreutils' shared library — one discipline, and the probe it delegated"
parent: moc-userspace-tools
code:
  - usr/coreutils/src/lib.rs
  - usr/coreutils/src/size.rs
  - usr/coreutils/src/meta.rs
  - usr/coreutils/src/ui.rs
  - usr/coreutils/src/usage.rs
  - usr/coreutils/src/netpump.rs
  - usr/coreutils/Cargo.toml
audit: none
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The nine modules the 51 coreutils binaries share: a colour palette and the
gate that turns it off, box-drawing furniture, human-readable sizes,
metadata presentation, a card renderer, `--help` plumbing, and the `/net`
byte pumps.

It exists to carry **one rule** across 51 programs: colour belongs on
presentation and on diagnostics, and never on a payload another program
reads. A coloured payload corrupts `tool | tool`, so the rule is the
difference between a suite of tools and a suite of things that look like
tools.

## Contract

Split in two by what a module needs. Four modules are pure — palette,
colour gate, size formatting, box furniture — with no syscalls and no
runtime dependency. Five are backend-gated behind a Cargo feature because
they touch the runtime: metadata presentation, the card renderer, the
help/usage plumbing, and the network pumps.

That split is not organisational. It is what lets the pure half be tested
on the host at all (see Caveats), and it is what makes the colour rule
enforceable: a filter that never names the colour modules cannot emit an
escape sequence.

The colour gate's shape is the load-bearing part. `col(code, on)` returns
the escape or the empty string, so one formatting expression is correct in
both modes and byte-identical to the uncoloured text when off. There is no
second code path to keep in sync — which is exactly how a "clean output"
mode usually rots.

## Mechanism

**The gate resolves once per run, from a flag, with the terminal probe
supplied by the caller.** `ColorMode` parses `--color=WHEN` — always,
never, or auto — and `resolve` takes the probe as a closure. The library
deliberately does not implement the probe, because answering "is stdout a
terminal" needs a syscall and the pure half has none.

That delegation is the single most consequential decision in the module,
and its consequence is described in [[sub-coreutils-presenters]]: every
one of the fifteen callers supplied the same stub.

**Box furniture computes on plain text and colours at emit.** A box is an
exact number of visible columns on every line; colour spans are
zero-width, so sizing must happen before they are inserted. The card
renderer carries both strings per row for precisely this reason — one
sizes, one prints. Get it backwards and every framed listing tears the
moment a name is coloured.

**Size formatting is integer-only**, because there is no float in this
environment: one decimal below ten of a unit, none at or above, with the
rounding carry handled explicitly so 9.96 K becomes 10 K rather than 9.10 K.

**Metadata presentation classifies an entry by what failed.** A directory
that `readdir` reports but `fstat` cannot cross is a *graft* — a live
kernel namespace mount. The failure is the signal, which turns what would
be an unexplained error row into a first-class kind with its own colour
and its own realm column.

**The network pumps encode one non-obvious fact about the network
daemon**: its data write is non-blocking, so a full send window returns a
zero-count write rather than deferring. A naive write-everything loop
therefore fails the instant the first window fills — which is exactly the
bulk transfer these tools exist to perform. The pumps wait on a
writability signal and retry, one in-flight chunk per direction.

## Data structures

Small and mostly stateless. A colour mode enum, a card row carrying its
plain and coloured forms, an entry-kind enum, and — in the pumps — a
fixed staging buffer with a sent/pending cursor plus half-close
bookkeeping, one per direction.

The staging buffer is deliberately fixed and stack-resident: these are
short-lived programs with a small heap, and a per-transfer allocation
would be the wrong trade.

## Concurrency

None. Every binary is single-threaded; the pumps multiplex with a poll
loop rather than threads. The pure modules have no state at all.

## Invariants enforced

None of the numbered system invariants. This is a presentation library
beneath every boundary — no capability, no syscall in the pure half, and
in the gated half only reads and writes on descriptors the caller already
holds.

Its own rule — colour on presentation and diagnostics, never on a payload
— is not enforced by a check anywhere. It is enforced by **module
linkage**: a tool that does not name the colour modules cannot colour its
output. [[sub-coreutils-filters]] records that this holds exactly, across
thirty-six programs.

## Error paths

The help plumbing implements the GNU convention deliberately: `--help`
anywhere before a `--` terminator prints usage to stdout and exits zero; a
usage error prints to stderr with a "try --help" hint and exits two.
Position-independence matters — a user types `--help` at the end as often
as at the start.

The dial-string resolver distinguishes an unresolvable name from a
malformed one, which is the difference between "no such host" and "you
typed the address wrong".

The pumps treat a read error on a socket as end-of-stream rather than as a
failure, on the reasoning that a connection that has stopped producing is
finished either way. That is a judgement call, and it means a genuine
transport error and a clean close are reported identically.

## Performance

Irrelevant at this layer with one exception: the box-fitting pass walks
every row to find the widest before drawing, so a listing is measured
twice. For directory listings that is free.

The pumps size their staging above the network daemon's send window so a
read rarely straddles a chunk boundary — a deliberate constant, not a
guess.

## Prosecution

- **The colour gate must stay a single formatting path.** The value of
  `col(code, on)` is that there is no separate uncoloured branch to drift.
  A tool that grows an `if on { ... } else { ... }` pair has reintroduced
  the failure mode the gate exists to prevent.
- **Box width must keep being computed on plain text.** Sizing on a
  coloured string counts escape bytes as columns and tears the frame.
- **The pumps must keep treating a zero-count write as back-pressure.**
  Treating it as an error is the documented naive failure, and the
  runtime's write-everything helper does exactly that — so the two must
  not be confused at a call site.

## Seams

A degrade to 256-colour or 16-colour terminals is unbuilt; the palette
emits truecolour only.

There is no uid-to-name service, so the owner column prints `system` for
the kernel principal and a bare number for everyone else.

The entry-kind enum has no symlink variant while the permission-string
builder has an `l`, so a symlink classifies as a plain file for colour and
suffix purposes while its mode string reports it correctly.

## Caveats

- **Its tests run, and the command its own header gives for running them
  does not.** The four pure modules carry fifteen tests, and they pass —
  verified by building. But the crate front door documents the invocation
  without the flag that drops the runtime dependency, and that invocation
  fails: it tries to compile the runtime for the host and dies in the
  startup assembly. The sibling manifest describes the same procedure
  correctly. So the file that tells you how to run the tests is the one
  that gets it wrong (task #157).

- **Fifteen tests cover four modules of nine.** The pure half is
  well-covered — box geometry is asserted to exact widths, the size
  rounding carry has its own case, the colour gate is checked in both
  states. The backend-gated half — metadata classification, the card
  renderer, the help plumbing, and all 450 lines of network pumping — has
  none, because it cannot be built for the host at all.

  That matters most for the pumps, which are the subtlest code in the
  module and the only part with a state machine: two independent
  half-duplex legs, a half-close that must propagate once each direction
  drains, and a termination condition. The splice loop's
  "nothing left to wait on" branch is in fact unreachable — the four
  conditions that would produce it imply the loop already returned — so it
  is a defensive guard, correctly written, that no test will ever enter.

- **The front door describes a 16-tool suite; there are 51.** The manifest
  beside it says "the ~50 coreutils bins" and is right. A construction
  snapshot, harmless, but it is the second claim in the same crate that
  the file next door contradicts.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
