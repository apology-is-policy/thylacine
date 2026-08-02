---
id: chg-2026-08-02-registry-pass
type: chg
title: "the enumerated registries -- and a guard that cannot fire"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - abi-errno
  - abi-caps
  - abi-handle-rights
  - abi-note-names
  - view-spec-coverage
  - arc-vault
established:
  - abi-errno
  - abi-caps
  - abi-handle-rights
  - abi-note-names
  - view-spec-coverage
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/err.rs"
  - "usr/lib/libthyla-rs/src/lib.rs"
  - "usr/lib/libthyla-rs/src/handle.rs"
  - "usr/lib/pouch/patches/0001-pouch-syscall-seam.patch"
  - "usr/lib/pouch/patches/0007-pouch-signals.patch"
  - "usr/lib/pouch/patches/0021-pouch-pty.patch"
  - "stratum src/block/bdev_thylacine.c"
depth: skeletal
created: 2026-08-02
---
Batch 24, the first half of the registry pass batch 23 identified as a
PREREQUISITE for finishing absorption rather than a successor to it. Main had
not moved (`631c8ade`), so the branch was already synced; L-1 checked for the
TWELFTH time and still absent (`1ac3628a` is real but sits on a branch main
does not contain).

Four enumerated-value registries, read from the headers and cross-checked
against every mirror in the tree: **errno** (20 values), **note names** (9
deliverable + 7 reserved), **capability bits** (12), **handle rights + kobj
kinds** (6 rights, 12 kinds, 4 partitions). Before this the boundary plane
held exactly one note, `abi-boot-banner`, and it is a contract rather than a
table -- so a document that is mostly table had nowhere to be absorbed to.

**F1 -- A GUARD THAT CANNOT FIRE, AND ITS COMMENT SAYS OTHERWISE.** `caps.h`
carries

    _Static_assert(CAP_ALL == (CAP_HW_CREATE | ... | CAP_GRANT_CLEARANCE),
                   "caps.h drift: when adding a new FORK-GRANTABLE CAP_* bit,
                    update CAP_ALL so kproc's initial mask reflects it.");

whose right-hand side is `CAP_ALL`'s own definition, token for token. The
comparison is `X == X`. It is unconditionally true and **cannot fail**, so
the drift its comment describes is precisely the drift it does not catch.
MEASURED, not read: a standalone reproduction defining a thirteenth
fork-grantable bit and omitting it from `CAP_ALL` -- the exact mistake --
compiles clean.

The consequence is fail-CLOSED, which is why it could sit undetected: a
forgotten `CAP_ALL` update makes the new capability ungrantable rather than
over-granted, so it reads at runtime as "the feature does not work." The fix
is the coverage assert `handle.h` already has. Task #35.

**THE CONTRAST IS THE FINDING, NOT THE BUG.** `handle.h` and `caps.h` have
the SAME two-set partition shape. `handle.h` guards it with seven assertions
-- six pairwise-disjointness plus a real **coverage** assert that the union
of the four kind masks equals every defined bit except `KOBJ_INVALID` -- so
a new kind in two masks fails the build AND a new kind in no mask fails the
build. `caps.h` has the disjointness half and a tautology where the coverage
half should be. Today `CAP_ALL` and `CAP_ELEVATION_ONLY` happen to partition
all twelve bits exactly six and six; nothing holds that in place.

**F2 -- `RIGHT_ALL` IS AN UNPINNED LITERAL, AND ITS FAILURE MODE IS WORSE.**
`0x3fu`, hardcoded, with nothing tying it to the six `RIGHT_*` bits. A
seventh right without a bump compiles clean and then fails *at runtime, from
six sites*: `RIGHT_ALL` is the validation mask, so `handle_alloc` and five
`syscall.c` gates all reject any request carrying the new bit as
out-of-range. Unlike the capability case -- where forgetting means nothing
happens -- forgetting here means every attempt to USE the right fails
validation with nothing pointing at the cause. Task #36.

**F3 -- THE ERRNO MIRROR LAGS BY EXACTLY THE APPENDS.** `err.rs` enumerates
15 of the 19 non-zero values. Missing: `T_E_SRCH` (3), `T_E_NODEV` (19),
`T_E_OPNOTSUPP` (95), `T_E_CANCELED` (125) -- precisely the set appended
after the mirror was written. Nothing loses information (`Other(i32)` is a
deliberate pass-through) but nothing NAMES them either: `setpgid` on a
stranger's pid and a cancelled Loom chain op are both live emit sites and
both surface as `Error::Other(n)`, matchable only against a magic number.
The kernel's discipline is an assert per value; the Rust side has no
equivalent and no test compares the lists, so four appends went without a
mirror update and the build stayed green each time. Task #34.

**A MIRROR THE FIRST DRAFT UNDERCOUNTED, CAUGHT BY THE LINTER'S OWN R6
RULE.** `abi-note-names` was written claiming ONE pouch mirror
(`0007-pouch-signals.patch`). Checking it -- because R6 refused the change
note until every touched abi's mirrors were accounted for -- showed the
mapping is split across TWO patches, the tty family living in
`0021-pouch-pty.patch`. Recording a mirror set is only as good as opening
each one, which is the same lesson as the path-mention screen in batch 23:
a plausible list is a screen, not a verdict. The check also surfaced that the
mapping is many-to-one at SIGINT/SIGTERM -- both land on `interrupt`, so a
pouch handler cannot distinguish a Ctrl-C from a termination request.

**THE THEME, WHICH IS BATCH 23'S ONE LAYER DOWN.** Batch 23 found that a
stub's state is guarded and its prose is not. Here the same split appears in
the code: **the assertions pin the values, and nothing pins the description
of the values.** Three headers carry stale counts, each from an append that
updated the macro and not the sentence beside it -- `caps.h` says "all five"
and lists six, and enumerates four of the six elevation-only bits;
`handle.h` says "nine kobj kinds" in one place and "eight kinds" in another
where the asserted count is twelve, and describes two partitions where there
are four; `sys_postnote_handler` calls four names "the v1.0 supported set"
where the set is nine (harmless in consequence -- those four ARE the
userspace-postable subset -- but wrong as written). None misleads the
compiler. All mislead a reader, and one of them (`19-handles.md`, which
still cites `KOBJ_KIND_COUNT == 9`) has already propagated into the
reference document.

**A CORRECTION TO BATCH 23'S OWN DIAGNOSIS.** That batch said the twelve
prose-swept documents are blocked on "a boundary registry", and named
`19-handles.md` among them. Reading its tables shows the blocker is a
different missing note type: they are return-code tables (which belong in a
dossier's Error paths section) and a **spec action-to-site map**, which per
the schema belongs to a `spec` note. `handles.tla` has none. So the blocker
set is at least two kinds, and for that document batch 23 pointed at the
wrong one.

Checkable, so computed rather than asserted: **[[view-spec-coverage]]** reads
`specs/` and reports 27 dossiered, 6 missing of 33 modules -- `asid`,
`burrow`, `debug_step`, `handles`, `pty`, `tapestry_present`. Stating that
in prose is the shape that let the absorption ledger rot for twenty batches;
the view cannot fall behind the tree.

**WHAT THIS BATCH DELIBERATELY DID NOT DO.** No document was stubbed. The
enumerated registries are one of at least three table categories; the STRUCT
layouts (`t_stat` and its six mirrors, the Loom ring structures, the 9P wire
structures) are the second and are what actually unblocks `107-loom.md`, and
the spec notes above are the third. Claiming absorption on the strength of a
partial registry pass would repeat exactly the error batch 23 found.

PROBE. Three, each asserted on disk before linting, restored from copies
taken after the last real edit.

**P1** -- the `CAP_ALL` tautology, probed in C rather than in the vault: a
thirteenth fork-grantable bit defined and deliberately omitted from
`CAP_ALL` compiles clean, confirming the assert is vacuous. This probe IS
the finding; without it F1 would be a reading of the preprocessor rather
than a measurement of it.

**P2** -- add a `.tla` module and lint WITHOUT re-rendering: caught, stale
generated body, so the coverage count cannot drift from the tree.
Deliberately additive rather than a deletion: another session is running the
interactive suite against the main worktree, and although nothing in it
reads `specs/`, an added file is inert where a removed one might not be. The
probe stayed inside the vault worktree, which carries its own full checkout
-- the renderer reads that copy, not main's.

**P3** (control) -- gut a spec note's body entirely, keeping only its
frontmatter, and re-lint: **passes**, and the view still reports it
`dossiered`. That is the view's deliberate limit, stated in its own text: it
checks that a note EXISTS, never that it is complete or still true of its
module. So a spec note can rot against its `.tla` and this will not say so
-- the same shape as batch 23's unguarded stub prose, and worth naming
rather than discovering later.

LEDGER. Boundary plane 1 -> 5 notes. Views 6 -> 7. Absorption unchanged at
46/101/147, deliberately.
