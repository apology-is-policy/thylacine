---
id: sub-coreutils-filters
type: sub
title: "The filters — thirty-six tools kept byte-clean by not linking the module"
parent: moc-userspace-tools
code:
  - usr/coreutil-smoke/src/main.rs
  - usr/coreutils/src/bin/aurora-push.rs
  - usr/coreutils/src/bin/basename.rs
  - usr/coreutils/src/bin/cat.rs
  - usr/coreutils/src/bin/chmod.rs
  - usr/coreutils/src/bin/clear.rs
  - usr/coreutils/src/bin/cmp.rs
  - usr/coreutils/src/bin/cp.rs
  - usr/coreutils/src/bin/cut.rs
  - usr/coreutils/src/bin/date.rs
  - usr/coreutils/src/bin/dirname.rs
  - usr/coreutils/src/bin/echo.rs
  - usr/coreutils/src/bin/env.rs
  - usr/coreutils/src/bin/false.rs
  - usr/coreutils/src/bin/head.rs
  - usr/coreutils/src/bin/hexdump.rs
  - usr/coreutils/src/bin/id.rs
  - usr/coreutils/src/bin/mkdir.rs
  - usr/coreutils/src/bin/mv.rs
  - usr/coreutils/src/bin/pwd.rs
  - usr/coreutils/src/bin/realpath.rs
  - usr/coreutils/src/bin/rm.rs
  - usr/coreutils/src/bin/rmdir.rs
  - usr/coreutils/src/bin/seq.rs
  - usr/coreutils/src/bin/sleep.rs
  - usr/coreutils/src/bin/sort.rs
  - usr/coreutils/src/bin/tail.rs
  - usr/coreutils/src/bin/tee.rs
  - usr/coreutils/src/bin/touch.rs
  - usr/coreutils/src/bin/tr.rs
  - usr/coreutils/src/bin/true.rs
  - usr/coreutils/src/bin/uname.rs
  - usr/coreutils/src/bin/uniq.rs
  - usr/coreutils/src/bin/wc.rs
  - usr/coreutils/src/bin/which.rs
  - usr/coreutils/src/bin/whoami.rs
  - usr/coreutils/src/bin/yes.rs
audit: none
guarded-by: []
validated-by: [prose, gate-smp, gate-interactive]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-04
updated: 2026-09-24
---
## Purpose

Thirty-six of the fifty-two coreutils binaries: the text filters, the file
operations, and the small identity and time queries. Everything whose
output another program is expected to read.

They are grouped here by a property that is measured rather than
asserted — **none of them links the colour modules.** That is what keeps
`tool | tool` byte-clean, and it is the strongest form the crate's colour
rule could take.

## Contract

Each is a standalone binary with its own entry point and allocator,
compiled from one file. It parses its own arguments, does one job, and
exits with a status: zero on success, one on a runtime failure, two on a
usage error. A reader that goes away is not a failure: once a write finds the
reader gone (EPIPE), a filter stops reading, writes nothing to stderr and exits
with the status it had, so `yes | cut -c1 | head -1` ends and `cat big | head
-1` stays clean under ut's pipefail. Any other failed write is reported on
stderr as a write error (`cat`, `head` and `tail` add its cause) and exits one,
and the tool stops there, since nothing more reaches stdout. A banner is
written the way the payload is, so it fails the same way. Three kinds of tool
differ. `tee` goes on copying its input to its files once stdout has failed,
as POSIX has it, and stops early only when every file has failed too.
Fifteen one-shot tools here (`basename`, `clear`, `cmp`, `cp`, `date`,
`dirname`, `echo`, `id`, `mkdir`, `mv`, `pwd`, `rm`, `uname`, `which` and
`whoami`) still write through the swallowing `io::out` or `print!`, so a
failed write is neither reported nor seen in their status. And the producers
`seq` and `yes` stop at any failed write, as they must when the reader goes,
but report none and exit zero, so `seq 3 > /dev/full` succeeds (Caveats).

There is no shared argument parser — each tool reimplements its own flag
loop against the runtime's argument iterator, which is why the flag
*conventions* are consistent while the parsing code is not shared.

## Mechanism

**The byte-clean property is structural, not checked.** The library's
colour modules must be named to be used; a binary that never writes
`coreutils::palette` cannot emit an escape byte. Verified across the set:
zero of these thirty-six reference the colour or palette modules, and all
sixteen of the others do. The partition is exact.

That is the same "authority by absence" move the runtime libraries use for
capabilities, applied to output cleanliness — and it is stronger than a
gate, because a gate can be forgotten at one call site while a missing
import fails to compile.

**Transform flags do not break it.** `cat` copies bytes on the plain path,
and for `-n`, `-b`, `-s`, `-E`, `-T`, `-v`, `-A` passes each read through
`coreutils::stream::CatLines`, which transforms it as it arrives and holds no
line, so a line with no end (`cat -v disk.img`) goes through in bounded memory.
Both are plain text: the second is a *user-requested* transform, not
decoration, so it stays pipe-safe. The line counter and the blank-squeeze
state run continuously across every operand rather than resetting per file,
which is the behaviour that makes `cat -n a b` number the concatenation
instead of each file.

**Move is a rename, not a copy.** It uses the runtime's rename, which maps
onto the filesystem's atomic replace. Same-device only, which today is not
a restriction: the whole pivoted tree is one device.

**Copy is open, create-truncate, copy.** Recursion skips the two dot
entries the directory reader yields — a real hazard handled, since a
recursive copy that followed the parent entry would walk upward.

**`mkdir -p` treats an uncreatable-but-existing ancestor as success.**
Walking the chain from `/`, `-p` tries to create every ancestor; one that
already exists inside a directory the user cannot write answers
permission-denied, not exists — the kernel checks the parent's write bit
before it discovers the child is already there. So the benign case is not
"the create returned EEXIST" but "the component is a directory afterwards",
re-checked after the failed create, which also absorbs a concurrent creator.
Without it, `-p` from `/tmp` or from a home died on its first existing
ancestor.

**Sort holds everything in memory; the rest stream.** Whole-line lexical by
default, with field and character keys, per-key modifiers, and a whole-line
last resort so the order is total. Sort cannot answer before its last line, so
it reads its whole input: whatever fits, since the heap grows until memory runs
out (B-1c); past that it ends at a fault, exit status 1, which is its error
status. `cmp`, `wc`, `cut`, `uniq` and `tail` read through `coreutils::stream`
([[sub-coreutils-lib]]) instead: `cmp` two buffers at their own pace, `cut` a
line at a time, `uniq` two (the line and its run's first), `wc` a buffer at a
time with the word state carried across reads, `tail` a window of its answer, or for
`+N` nothing: it skips to line or byte N and copies the rest as it reads, as
POSIX has it (`+0` is `+1`, as in GNU's). `uniq` prints a run's line as the run
begins, as GNU's plain `uniq` does, unless `-c`, `-d` or `-u` must see the
run's end. `cut`
hands on each selected field as `coreutils::select` finds it, never a list of
the line's fields, which could outgrow the line many times over. Until
B-1c they slurped into the fixed heap. Streaming is what keeps `cmp`'s exit 1 a
verdict, since a fault kill also exits 1; and an empty input now has no lines,
so `cut` and `uniq` of nothing print nothing. A line longer than 64 MiB
(`LINE_MAX`) is refused: `cut`, `uniq` and `tail -n N` report "a line longer
than 64 MiB" and exit one (`tail -c` and `tail +N` hold no line). An input
with no end is read for as long as it lasts, in bounded memory: `wc`, `cmp`
and `tail -c` of `/dev/zero` run until interrupted, as GNU's do, where the
slurp stopped at its 2 MiB cap with an error; `tail -n 0`
reads nothing at all, as GNU's does. `head` and `tail` read standard input for
an operand of `-`, and name it `standard input` in a banner. `--` ends their
options, so an operand after it may begin with a dash; `tail -n -N` is
`tail -n N`, POSIX's explicit sign for counting from the end; and a count too
large to hold is refused with `invalid count` and exit one, in every form
either takes.

**Path cleaning and input streaming are the shared behaviour.** `realpath` no
longer carries its own lexical normaliser; the collapse of `.`, `..` and `//`
lives in `coreutils::path::normalize`, so a filter and the colour-linking
presenters (`ls`, `ps`, `stat`) that emit a cleaned-absolute reference clean it
the same way. The input loops in `coreutils::stream` are the other piece:
`grep`, a presenter, reads through the same `lines` as `cut` and `uniq`. They
are the only *logic* — as against the shared `--help` and usage-error plumbing
— that crosses the partition: a path is a path on both sides, and neither
module can emit a colour byte.

**The odd one out is aurora-push**, which is a filter in the linkage sense
— no colour modules — but writes terminal escapes as its *entire purpose*:
it emits the renderer's private settings sequence on stdout, which the
compositor drains out of the console byte stream. It is grouped here
because it links nothing from the palette, and because its escapes are a
protocol to a specific consumer rather than decoration of a payload. Each
push is preceded by a reset verb so the result is deterministic — system
defaults plus this user's overrides — and a stale push from a prior
session cannot survive the next login.

## Data structures

Almost none. Each tool holds a small flags struct and whatever buffer its
job needs. Sort holds the full line vector; the rest stream -- through
`coreutils::stream` where the input is unbounded, holding a line, two buffers,
or `tail`'s window (nothing, for `tail +N`).

## Concurrency

None. Every one is single-threaded and short-lived.

## Invariants enforced

None. These are the least privileged programs in the tree: they hold no
capability, own no device, mediate nothing, and act only on descriptors
and paths their namespace already grants. A defect corrupts the invoking
user's own files at the invoking user's own authority — which is the
correct blast radius for `rm`.

The filesystem permission checks that bound them are the kernel's, applied
at every walk and open. `chmod` is the one that *changes* metadata, and its
authority is the kernel's identity check, not anything in this file.

## Error paths

Diagnostics go to stderr with the tool's name as prefix; usage errors exit
two with a "try --help" hint, per the shared plumbing. Runtime failures
exit one.

A per-operand failure does not abort the run where continuing is right — a
`cat` of three files reports the unreadable one and still emits the other
two — which matches the convention users have.

A streaming filter has printed what it read before a read error: `cut` passes
each line on as it arrives and `uniq` each run's line as the run begins (under
`-c`, `-d` or `-u`, once the next begins), so an error part-way through leaves
what came before it on stdout (a `uniq` waiting on a run's end drops the run
still open, as GNU's does), then the diagnostic and status one. `cat`,
`head` and `tail` tell a failed read from a failed write: the first names the
input and goes on to the next operand, the second is a write error with its
cause and ends the run. `cmp` opens both
files before it reads either, and a difference or a shorter file found before a
read error decides its status, where the slurp read both whole first and a read
error anywhere gave two.

## Performance

Streaming where it can be, which is most of them. Sort is the exception by
design.

## Prosecution

- **The colour partition must stay exact.** A filter that grows a coloured
  header has become a presenter and belongs with them; adding the import
  is the moment to move it, because after that nothing structural prevents
  it from colouring a payload.
- **Recursive walks must keep skipping the two dot entries.** The
  directory reader yields them, and a copy or remove that followed the
  parent entry would leave the subtree.
- **A gone reader must stay silent and status-neutral, and must stop the
  reading.** A filter that reports EPIPE fails an innocent pipeline under
  pipefail, and one that keeps reading after its stdout failed never ends on an
  endless producer. `coreutil-smoke`'s reader-leaves checks pin both halves for
  `grep`, `cut`, `cat`, `tee`, `uniq` and `tail -n +1`, and a reader gone
  before the first write pins `tail`'s banner.
- **Every write goes through the failure path, framing included.** A banner
  written through the swallowing `print!` let `tail` go on to read an endless
  next input with no reader, and let a failed banner exit zero; the smoke
  points `head` and `tail` at `/dev/full` to hold both.
- **Rename must keep being a rename.** Falling back to copy-then-remove on
  a cross-device move would silently lose the atomic-replace property that
  callers depend on.

## Seams

Cross-device move is unbuilt — currently unreachable, since the tree is
one device, but the fallback is named as a later refinement.

There is no regular-expression engine, so the pattern tools take literal
substrings; `grep` says so in its own usage text.

The entry-kind vocabulary has no symlink case, so tools that classify
report a symlink as a plain file even where the mode string shows `l`.

## Caveats

- **`which` answers from a mirror that has drifted, and its own header
  says drift is a bug.** The shell resolves a bare command against a
  six-entry list — `/bin/`, `/`, `/goroot/bin/`, `/clade/bin/`, `/viv/bin/`,
  `/viv/abin/` — while the environment variable `which` reads is seeded with
  five, dropping only the namespace root `/`. So a binary that lives at `/`
  (the pre-pivot initrd root, where the boot-test shell runs) *runs* when
  typed and reports *not found* when asked about. The lists have since grown
  together: the `/viv/bin` instance of this drift — `git` ran while `which
  git` failed because the shell list carried `/viv/bin` and the login seed did
  not — was closed at X-2 (W1-b) by seeding both surfaces, and `/clade/bin` was
  added to both at once. The residual is the single `/` entry; the same
  omission stands against the shell's completion index (task #159).

- **The binaries have no host tests.** They are binary crates that link
  the runtime unconditionally, so `cargo test` cannot build them — the same
  wall [[sub-aurora]] hits, for the same reason. What is pinned: the input
  loops they share (`stream`, host-tested at every read boundary in
  [[sub-coreutils-lib]], `uniq`'s grouping among them), and `coreutil-smoke`,
  which joey runs every boot to feed most filters a fixed input and assert the
  exact output and status — including a line longer than the pipe and the read
  buffer, an empty input, `tail`'s window over 5000 lines and its signed
  counts, `--` in `head` and `tail`, a count too large for `head`, a line past
  `LINE_MAX` refused, `cat`'s transforms, and a reader that leaves: `yes` or
  `seq` feeding a filter whose output is dropped after two pipes' worth, which
  must then stop within 20 seconds, silent and with status zero, and the
  producer with it. A reader gone before the first write and a stdout on
  `/dev/full` pin the banners. Every check feeds its tool's input as the pipe
  takes it, reads its stdout and stderr as they come and kills a tool still
  running at the check's bound, so no tool can hold the boot; two checks hold
  the capture itself to that, with more than a pipe holds through `cat` and a
  `yes` cut off one second after its first output. What is not: most flag
  combinations, most error
  paths, and every boundary the smoke does not name.

  That is worth holding next to what these programs do. `rm`, `mv`, `cp`
  and `chmod` mutate the filesystem irrecoverably, and their argument
  parsing — which operand is the destination, whether a trailing name is
  an existing directory — is exactly the logic a test would pin. It is
  pinned by nothing.

- **Fifteen one-shot tools and two producers swallow a failed write.**
  `basename`, `clear`, `cmp`, `cp`, `date`, `dirname`, `echo`, `id`, `mkdir`,
  `mv`, `pwd`, `rm`, `uname`, `which` and `whoami` write stdout through
  `io::out` or `print!`, which drop a write error, so `echo data > /dev/full`
  exits zero where GNU's reports the error and exits one. None of them can hang
  on a gone reader, since each writes a bounded answer. `seq` and `yes` stop at
  a failed write, so neither outlives its reader, but exit zero whatever the
  failure was: `seq 3 > /dev/full` succeeds. The fix is `OutSink` and `finish`
  in each (`cmp` wants two on a failed write), a sweep owed after B-1c, with
  the network presenters' ([[sub-coreutils-presenters]]).

- **Thirty-six separate flag loops.** Consistency across them is
  maintained by hand. The `--help` and usage-error behaviour *is* shared,
  so the part a user notices first is uniform; everything below it is
  reimplemented per tool.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
