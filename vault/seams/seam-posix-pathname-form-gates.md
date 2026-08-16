---
id: seam-posix-pathname-form-gates
type: seam
title: "POSIX pathname-form gates — landed on the unmerged vivarium branch"
status: closed
surface: [sub-kernel-stalk]
opened-by: chg-2026-07-31-stalk-sweep
tracker: "tasks #79-#87 (vivarium numbering)"
created: 2026-08-01
updated: 2026-08-01
closed-by: chg-2026-08-16-stalk-posix-form
---
## Owed

This lineage's resolver still has the POSIX pathname-FORM gaps the
errno-loss family closed elsewhere: resolution THROUGH a file reports
NOENT (POSIX: ENOTDIR — a lie about WHY, never a containment breach);
`a/b/..` pops back through a file and `a/b/.` returns the file (`.`/`..`
are handled lexically before any type gate); a trailing slash is dropped
by the tokenizer so `/etc/passwd/` resolves the file; FS name-ops lose
their precise errno (the ER-2 walk-vtable out-param). The fix family is
LANDED but on the unmerged vivarium branch (`aux-2`): #79 `6790c125`
(ENOTDIR type gate), #80 `a6520c79` (ER-2 + an ER-3 slice), #81
`a4a7cedd` (dot-out-of-file, UNCROSSED tip — `/mnt/.` must equal `/mnt`
under STALK_MOUNT), #82 `83dd63c8` (trailing slash, THREE gate sites on
the CROSSED quarry — the two POUNCE/cached-open success exits never
reach the ordinary return), #83 `e06fb0a6` (cwd-relative parity), #84
`a0d146a7` (`.`/`..` need X where resolved), #86/#87 (the pouch/
libthyla-rs splitter halves), all 2026-07-29.

## What closes it

The vivarium branch merging into this lineage — the family's chgs then
enter the Record plane (with the #81-vs-#82 crossed/uncrossed design
pair and the four-way revert-probe coverage) and this seam closes. Do
NOT re-implement on this side.

## Risk while open

Errno fidelity + POSIX conformance only: scripts and ports that
distinguish ENOTDIR from ENOENT (or rely on `path/` asserting
directoriness) misbehave. I-28 containment and the X-search are
unaffected — the gaps lie about WHY a resolution failed or resolve a
form POSIX forbids, never WHERE resolution may reach.
