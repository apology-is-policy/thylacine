---
id: chg-2026-08-16-stalk-posix-form
type: chg
title: "stalk: the POSIX form gates merged, and a gate binds only what it sees"
date: 2026-08-16
arc: arc-vault
commits: ["79a93f65"]
touched: [sub-kernel-stalk]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The pathname-form family — six commits authored 2026-07-29/30 on the vivarium
branch — is now an ancestor of this lineage, verified per-commit rather than
inferred from a merge date. [[seam-posix-pathname-form-gates]] closes on
exactly the condition it named.

## Four gates, one ordering rule

They read as one design rather than four patches because a single rule governs
every one: **type before permission, always.** The x bit on a non-directory
says nothing about whether it can be traversed, so a permission-first order
answers `EACCES` for a path that can never resolve *as written*. A `0000`
regular file is `ENOTDIR`; an unreadable `file/` is `ENOTDIR`, not `EACCES`.
And for `..` there is a second ordering: the search check runs **before the
pop**, while the directory being popped out of is still the subject.

## The pair that must not be unified

Two gates read `qid.type` and disagree about crossing, on purpose:

- the dot gates read the tip **uncrossed** — `.`/`..` are about **where
  resolution stands**, so `/mnt/.` must equal `/mnt`, and under `STALK_MOUNT`
  the mount point deliberately does not cross;
- the trailing-slash gate reads the quarry **crossed** — a trailing slash is
  about **what the quarry is**, so a directory mounted over a file legitimately
  makes `file/` resolve.

Same field, opposite subjects, both right. A future reader tidying them into
one helper breaks whichever loses, silently, in a case neither test covers
directly. This is the kind of thing that has to be written down in the dossier
because the *code* cannot express "these two look identical and must stay
apart".

The dot gates also had to be written separately from the through-a-file gate
for a structural reason: `.` and `..` are handled by stalk itself and **never
reach `Dev.walk`**, so a gate on the real-component arm cannot see them.
`a/b/..` popped back to `a` and `a/b/.` handed back `b` while `/bin/ls/foo` was
already correctly refused.

Containment is **strengthened, not touched**. At depth 0 the dot gate's subject
is `start`, and `ENOTDIR` there is strictly more restrictive than the old
no-op: the gate can only turn a success into a failure, never move a pop
further up. [[inv-i28]] gains, and nothing about it needed re-arguing.

## A gate binds only what it SEES — the finding that outranks the rest

All four gates live in stalk, so they bind only the paths that reach stalk. The
cwd join **resolved `.` and `..` and dropped a trailing separator before
calling it**, so every gate was bypassed for the commonest path form in a
shell.

Seven consequences, **measured on the pre-fix tree** rather than argued, from a
working directory holding a regular file `f`: `open("f/..")`, `open("f/.")` and
`open("f/")` all returned working descriptors; `stat("f/")` and `stat("f/..")`
succeeded. The two that make it a resolution bug rather than a conformance nit:
**`open("nope/..")` returned a working fd** — a lexical `..` pops a component
without proving it exists, so a path traversing a directory that is not there
opened — and **`chdir("f/..")` succeeded**, having run its directory check
against the parent it had already massaged the path into. Checking the wrong
object entirely.

**The fix was a unification, not a fifth gate.** Joining and canonicalising had
been one function; they were separated, leaving one production caller of the
canonicalising half.

That is the same move [[sub-libthyla-rs]] made in the same window, and the pair
is the argument for the principle rather than for either fix: **three layers
independently normalised paths — this join, the ported libc's splitter, the
native runtime's — each was wrong differently, and all three were repaired by
DELETING the normalisation rather than correcting it.** When N layers each
clean a path, they each clean it wrong, and the resolver's gates never see what
they exist to judge.

## Twice, the enqueued mechanism was bettered by reading the code

Worth recording as a pattern in how these tasks get written, not as praise.

The through-a-file task proposed an **errno out-param on `Dev.walk`** to
transport a Dev's `ENOTDIR` out to userspace — eighteen vtable signatures. The
resolver does not need to be told: it already holds the parent Spoor, and every
Spoor carries a qid whose `QTDIR` bit exists for exactly this. `dev9p` says so
in as many words. **The bit was put there for this check and the check was
never written.** Five lines, no vtable change, and it covers every Dev at once
instead of only the ones that learn to report.

The errno-loss task predicted the same shape — this seam records it as "the
ER-2 walk-vtable out-param" — and the implementation chose a **return-value
contract change** instead: both name-op slots return a specific `-T_E_*` rather
than a flat `-1`, with a NULL slot answering `OPNOTSUPP`, distinct from any
verdict an impl can return.

A task filed at discovery proposes the fix visible from *outside* the code. The
fix from inside is usually smaller, and twice in one family it was a mechanism
that already existed and had simply never been used. The seam's *description*
of the mechanism was wrong; the *defect* it named is closed, which is the right
thing for a seam to be judged on.

## And twice, a task named one token where the measurement showed two

Both the type gate and the search gate were filed against `..` alone. In every
measured row `.` and `..` behaved identically, so both arms needed both gates.

The search gate's measurement is the better method note: it was taken against a
**POSIX host** — non-root, owner of a `chmod 000` directory `d` — rather than
derived from the standard's prose. The first row is the whole shape of the bug:
`stat("d")` succeeds because the lookup happens in `d`'s *parent*, while
`stat("d/.")` is `EACCES` because `.` is resolved *in* `d`. Pre-fix, `d000/..`
resolved while the sibling `d000/x` was denied — reaching `d` never required x
**on** `d`.
