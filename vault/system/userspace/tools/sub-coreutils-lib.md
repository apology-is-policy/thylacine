---
id: sub-coreutils-lib
type: sub
title: "coreutils' shared library -- the bins' side of the Beacon gate, after the cells language left"
parent: moc-userspace-tools
code:
  - usr/coreutils/src/lib.rs
  - usr/coreutils/src/path.rs
  - usr/coreutils/src/size.rs
  - usr/coreutils/src/stream.rs
  - usr/coreutils/src/find.rs
  - usr/coreutils/src/select.rs
  - usr/coreutils/src/counting.rs
  - usr/coreutils/src/beacon_gate.rs
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
updated: 2026-09-24
---
## Purpose

The modules the 51 coreutils binaries share, as they stand after the colour
language moved out. Until 2026-09-01 this crate owned the Bonfire visual
language -- the palette, the `col(code, on)` gate, the box furniture -- and
its whole reason for existing was to carry one rule across 51 programs.
That language is now [[sub-beacon]]'s cells tier, relocated verbatim; this
crate re-exports it (`pub use beacon::{boxd, color, palette}`, so every bin's
`crate::boxd` call site is unchanged) and applies it.

What remains here is the bins' side of the Beacon system plus the utilities
that were never presentation: lexical path canonicalization, human-readable
sizes, the loops the filters stream their input through, the per-bin Beacon
emission gate, metadata presentation, the card renderer, `--help` plumbing,
and the `/net` byte pumps.

The old discipline did not leave with the code -- it moved up a layer. Colour
belongs on presentation and diagnostics, never on a payload another program
reads; that rule is now enforced structurally in [[sub-beacon]] and applied
here at one chokepoint, `beacon_gate`.

## Contract

Five modules are pure and ungated -- `path`, `size`, `stream`, `find` and
`select` -- with no syscall and no runtime dependency, so they compile and test
on the host; a sixth, `counting`, exists only in the host tests. Five are
backend-gated behind a Cargo feature because they touch the runtime:
`beacon_gate`, `meta`, `ui`, `usage`, `netpump`.

The cells trio (`boxd`/`color`/`palette`) is re-exported, not defined: it
lives, and is tested, in [[sub-beacon]]. A reader chasing the colour gate's
single-formatting-path shape or the box-width-on-plain-text rule finds them
there now; this crate only consumes them.

## Mechanism

**`beacon_gate` is the crate's new centre** (BEACON.md 12.4). One call --
`resolve(flag)` -- composes the three inputs the emission gate needs: the
renderer's advertised tier (the `BEACON` environment value ut exports), the
Dev class of stdout (`SYS_FD_DEVCLASS`, so frames go only onto the interactive
console under Auto), and the tool's `--beacon=WHEN` flag. It is the
libthyla-rs-touching half that [[sub-beacon]] deliberately left to the caller,
so the pure crate stays host-testable. Two disciplines ride it: at the Rich
tier the emitting bin forces its colour gate off (the renderer's stylesheet
owns typography inside Beacon-structured output), and an `obj type=path` ref
is canonicalized through `path` -- a ref that cannot be canonicalized emits no
frame at all, plain text only.

**`path` is lexical only** (realpath's `-m -s` semantics): collapse `.`,
`..`, and repeated `/`, no symlink resolution and no existence requirement.
It is shared by `realpath` and by the Beacon emitters, because BEACON.md
12.2's obj rule requires an `obj type=path` ref to be the cleaned *absolute*
form -- a relative or dirty ref is a wrong ref.

**`size` is integer-only**, because there is no float in this environment:
one decimal below ten of a unit, none at or above, with the rounding carry
handled explicitly so 9.96 K becomes 10 K rather than 9.10 K.

**`stream` is how a filter reads an input of any length.** `lines` hands each
line to a closure and holds only that line (the start of one a read cut short
is carried in a single buffer); `runs` builds on it for `uniq`, handing over
each run of equal adjacent lines as its first line and a count once the run
ends, and `firsts` hands over each run's first line as the run begins, for a
`uniq` that needs no count; `compare` walks two inputs a buffer each, at their
own pace; `Tail` keeps a window of the last lines or bytes, drained only once
it holds twice what it keeps, and `Skip` passes on what follows the first N - 1
lines or bytes, holding nothing (`tail +N`). Each takes its reads as a closure
or its input a piece at a time, so all are pure and host-tested against reads
cut at every boundary, checked against the answer the whole-input code gave. The
module exists because slurping stopped being bounded: since B-1c the heap grows
until memory runs out, and a program that outgrows it ends at a fault, exit
status 1 (docs/ERRORS.md) -- which is `cmp`'s "differ" and `grep`'s "no
match". A streamed filter holds a line, and a line is bounded too: past
`LINE_MAX`, 64 MiB (POSIX lets a text utility refuse a line longer than its
LINE_MAX), `lines`, `runs`, `firsts` and a line-counting `Tail` stop with
`Error::TooLong` instead of
growing toward that fault, so `grep x /dev/zero` exits 2 and never reads as "no
match". The carried line grows by a fallible reservation that never passes the
bound, and a buffer that held a long line is given back once it holds more than
twice what the next line needs. A closure returns false to stop the reading,
which is how a filter stops once its reader has gone. Two behaviours changed
with streaming, both toward GNU: an empty input has no lines (the whole-buffer
split read one, so `grep -v x`, `cut` and `uniq` of nothing printed a blank
line), and `grep -l` stops reading at its first match.

**A bound on a line is not a bound on what is built from it.** `LINE_MAX` bounds
the bytes held, but a list derived from them can be many times larger: a match
or a field is two words, sixteen bytes, and a line holds as many of either as it
has bytes, so one line at the bound could ask for a gigabyte of positions
(`grep -o`, any coloured `grep` line, `cut -f`). So nothing here collects. `find::each_match` hands `grep`
each match of its literal (ignoring ASCII case, or bounded to whole words, on
request) as it is found; `select::fields` and `select::bytes` hand `cut` each
selected field or run of bytes; and `stream::CatLines` applies `cat`'s line
transforms -- numbering, squeezing blank runs, showing line ends, tabs and
nonprinting bytes -- a read at a time with no line held at all. What it carries
across reads is a line number, whether the last line was blank and whether a
line is open, and it carries them across operands too, so `cat -n a b` numbers
the concatenation. Each is host-tested against the answer the collecting code
gave, and the cost is measured, not argued: `counting` is a test-only global
allocator that counts what each thread asks for, and a mebibyte line of matches
or of delimiters is searched and cut with no allocation at all.

**`meta` classifies an entry by what failed.** A directory that `readdir`
reports but `fstat` cannot cross is a *graft* -- a live kernel namespace
mount. The failure is the signal, which turns what would be an unexplained
error row into a first-class kind with its own colour and realm column.

**`netpump` encodes one non-obvious fact about the network daemon**: its data
write is non-blocking, so a full send window returns a zero-count write rather
than deferring. A naive write-everything loop fails the instant the first
window fills -- which is exactly the bulk transfer these tools exist to
perform. The pumps wait on a writability signal and retry, one in-flight
chunk per direction.

**`ui` is the card renderer**, carrying both a plain and a coloured string per
row (plain sizes the box, coloured prints) per beacon's compute-on-plain /
colour-at-emit rule. It draws through the re-exported `boxd`/`color`/`palette`,
so netstat's connection table and the nslookup/ping/dial/bench cards share one
look with `ls -l`.

## Data structures

Small and mostly stateless: a card row carrying its plain and coloured forms,
an entry-kind enum, and -- in the pumps -- a fixed staging buffer with a
sent/pending cursor plus half-close bookkeeping, one per direction. The
staging buffer is deliberately fixed and stack-resident; these are short-lived
programs, and a per-transfer allocation would buy nothing. `stream` holds one
8 KiB read buffer (two for `compare`) plus the carried line, the current run's
first line or the tail window, and none of them holds a line past `LINE_MAX`;
`CatLines` holds three scalars and `Skip` two, and no bytes, and `find` and
`select` hold nothing. The palette and colour-mode types live in [[sub-beacon]].

## Concurrency

None. Every binary is single-threaded; the pumps multiplex with a poll loop
rather than threads. The pure modules have no state at all.

## Invariants enforced

None of the numbered system invariants. This is a presentation + utility
library beneath every boundary -- no capability, no syscall in the pure half,
and in the gated half only reads and writes on descriptors the caller already
holds.

The colour/payload rule it once owned is now [[sub-beacon]]'s, enforced there
by module linkage and applied here at `beacon_gate`. Its own remaining rules:
the box-width-on-plain-text discipline (in `ui`, via beacon's `boxd`), and the
Beacon obj=path canonicalization (`path` + `beacon_gate`) -- a displayed ref
must be the cleaned absolute form or no frame is emitted.

## Error paths

The help plumbing implements the GNU convention deliberately: `--help`
anywhere before a `--` terminator prints usage to stdout and exits zero; a
usage error prints to stderr with a "try --help" hint and exits two.
Position-independence matters -- a user types `--help` at the end as often as
at the start.

The dial-string resolver distinguishes an unresolvable name from a malformed
one -- the difference between "no such host" and "you typed the address
wrong". The pumps treat a read error on a socket as end-of-stream rather than
a failure, on the reasoning that a connection that has stopped producing is
finished either way -- a judgement call that reports a genuine transport error
and a clean close identically. An un-canonicalizable path ref emits no frame
rather than a dirty one.

`stream` stops for one of three reasons, each the caller's to report: the read
failed (`Error::Read`, carrying the reader's own error), a line outgrew what the
heap would give (`NoMemory`), or a line passed `LINE_MAX` (`TooLong`, displayed
as "a line longer than 64 MiB"). What came before has already been handed
over -- every line for `lines`, every run's first line for `firsts`, every
finished run for `runs`, which drops the run still open -- so a filter has
printed what it could.

## Performance

Irrelevant at this layer with two exceptions inherited from the cells tier:
the box-fitting pass walks every row to find the widest before drawing (a
listing is measured twice, free for directory listings), and the pumps size
their staging above the network daemon's send window so a read rarely
straddles a chunk boundary -- a deliberate constant, not a guess. `stream`
copies only a line a read cut short, and `Tail`'s draining moves a bounded
amount per byte fed and shrinks the window back once a long line has passed.

## Prosecution

- **A program whose exit status 1 is a verdict must not hold an unbounded
  input.** At v1.0 a fault past free memory also exits 1, so `cmp` and `grep`
  read through `stream`; a verdict-bearing tool that slurps reopens HT09.R4-F2
  as a silently wrong answer.
- **A held line must stay under `LINE_MAX`.** A streaming consumer that grows a
  line outside `hold`, or grows it with an infallible `extend`, reopens the path
  from an endless line to a fault that reads as a verdict.
- **A bound on a line must also bound what is built from it.** A list of the
  matches, fields or spans in one line costs sixteen bytes an entry, up to
  thirty-two times the line once a `Vec` has doubled; hand each to the caller
  as it is found, and show the cost with `counting` rather than argue it.
- **`firsts` must hand a run's line over as the run begins.** Handing it over
  at the run's end holds a plain `uniq` behind its input -- `yes | uniq` shows
  nothing -- and loses the open run on a read error, where GNU's has printed it.
- **A buffer's release must be tested where it is used.** The first test of the
  rule called the helper and stayed green with the call deleted; `lines_within`
  and `runs_within` take their buffers from the caller so a test reads the
  buffer the loop used.
- **The pumps must keep treating a zero-count write as back-pressure.**
  Treating it as an error is the documented naive failure, and the runtime's
  write-everything helper does exactly that -- so the two must not be confused
  at a call site.
- **`beacon_gate` must force the colour gate off at the Rich tier.** SGR
  inside Beacon-structured output collides with the renderer's stylesheet;
  the discipline is the emitter's to hold because the gate cannot reach back
  into the bin's colour decisions.
- **An un-canonicalizable `obj type=path` ref must emit no frame.** A dirty or
  relative ref is a wrong ref (BEACON.md 12.2); emitting it would point the
  menu's verbs at the wrong object.
- **Box width must keep being computed on plain text.** Sizing on a coloured
  string counts escape bytes as columns and tears the frame -- the rule lives
  in beacon's `boxd` now, but `ui` is the caller that must respect it.

## Seams

There is no uid-to-name service, so the owner column prints `system` for the
kernel principal and a bare number for everyone else.

The entry-kind enum has no symlink variant while the permission-string builder
has an `l`, so a symlink classifies as a plain file for colour and suffix
purposes while its mode string reports it correctly.

A degrade to 256-colour or 16-colour terminals is unbuilt; beacon's palette
emits truecolour only.

## Caveats

- **The cells tier and the bulk of the old test suite left for
  [[sub-beacon]]** (2026-09-01). This crate's host-testable surface is now the
  five pure modules and the test allocator, which carry forty-two host tests:
  five for size's rounding-carry cases and path's normalization, twenty-eight
  for `stream` (every line and run at every read size, a run's first line
  handed over as it arrives, the compare positions, the tail window, the skip
  to line or byte N, the line bound, a long line's room given back, and `cat`'s
  transforms under every flag combination and piece size), four each for
  `find` and `select`, and `counting`'s own control, which shows it counts. The colour-gate,
  palette, and box-geometry tests -- the majority of the fifteen this dossier
  once counted -- moved with their code and are beacon's now.

- **The backend-gated half remains untestable on the host.** `beacon_gate`,
  `meta`, `ui`, `usage`, and all 450 lines of `netpump` cannot be built for
  the host at all (they die in the runtime's startup assembly), so none carry
  unit tests. That matters most for the pumps, which are the subtlest code in
  the crate and the only part with a state machine: two independent
  half-duplex legs, a half-close that must propagate once each direction
  drains, and a termination condition. The splice loop's "nothing left to
  wait on" branch is in fact unreachable -- a defensive guard no test will
  ever enter.

- **The front door describes a procedure that does not run, and a suite size
  that is a snapshot.** The crate header documents the test invocation without
  the flag that drops the runtime dependency, and that invocation fails; the
  sibling manifest describes it correctly (task #157). Separately, a
  construction snapshot in the manifest still describes "the ~50 coreutils
  bins" against 51 -- harmless, but the second stale count in the crate.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
