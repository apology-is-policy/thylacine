---
id: sub-coreutils-filters
type: sub
title: "The filters — thirty-six tools kept byte-clean by not linking the module"
parent: moc-userspace-tools
code:
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
validated-by: [prose]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

Thirty-six of the fifty-one coreutils binaries: the text filters, the file
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
usage error.

There is no shared argument parser — each tool reimplements its own flag
loop against the runtime's argument iterator, which is why the flag
*conventions* are consistent while the parsing code is not shared.

## Mechanism

**The byte-clean property is structural, not checked.** The library's
colour modules must be named to be used; a binary that never writes
`coreutils::palette` cannot emit an escape byte. Verified across the set:
zero of these thirty-six reference the colour or palette modules, and all
fifteen of the others do. The partition is exact.

That is the same "authority by absence" move the runtime libraries use for
capabilities, applied to output cleanliness — and it is stronger than a
gate, because a gate can be forgotten at one call site while a missing
import fails to compile.

**Transform flags do not break it.** `cat` streams bytes through a copy
helper on the plain path and switches to a line-oriented path for `-n`,
`-b`, `-s`, `-E`, `-T`, `-v`, `-A`. Both are plain text: the second is a
*user-requested* transform, not decoration, so it stays pipe-safe. The
line counter and the blank-squeeze state run continuously across every
operand rather than resetting per file, which is the behaviour that makes
`cat -n a b` number the concatenation instead of each file.

**Move is a rename, not a copy.** It uses the runtime's rename, which maps
onto the filesystem's atomic replace. Same-device only, which today is not
a restriction: the whole pivoted tree is one device.

**Copy is open, create-truncate, copy.** Recursion skips the two dot
entries the directory reader yields — a real hazard handled, since a
recursive copy that followed the parent entry would walk upward.

**Sort holds everything in memory.** Whole-line lexical by default, with
field and character keys, per-key modifiers, and a whole-line last resort
so the order is total. Reading all input into memory is a stated choice,
appropriate for a system whose files are small and whose heap is fixed.

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
job needs. Sort holds the full line vector; the rest stream.

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
  three-entry list — `/bin/`, `/`, `/goroot/bin/` — while the environment
  variable `which` reads is seeded with two, dropping the namespace root.
  So a root-level binary (and several exist: the boot probes are spawned
  by root-anchored path) *runs* when typed and reports *not found* when
  asked about. The same missing entry is already filed against the shell's
  completion index, so one omission now stands in three places, each of
  which documents itself as mirroring the others (task #159).

- **Zero tests, and none are reachable from the host harness.** These are
  binary crates that link the runtime unconditionally, so `cargo test`
  cannot build them at all — the same wall [[sub-aurora]] hits, for the
  same reason. Their proof is that the shell test scenarios run some of
  them on a live console every boot, which covers the paths a person types
  and nothing else: no flag combination, no error path, no boundary case.

  That is worth holding next to what these programs do. `rm`, `mv`, `cp`
  and `chmod` mutate the filesystem irrecoverably, and their argument
  parsing — which operand is the destination, whether a trailing name is
  an existing directory — is exactly the logic a test would pin. It is
  pinned by nothing.

- **Thirty-six separate flag loops.** Consistency across them is
  maintained by hand. The `--help` and usage-error behaviour *is* shared,
  so the part a user notices first is uniform; everything below it is
  reimplemented per tool.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
