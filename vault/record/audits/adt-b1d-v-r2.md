---
id: adt-b1d-v-r2
type: adt
title: "B-1d-v round 2: the fixes hold; devsrv aliased every /srv node onto the registry root's mount key"
date: 2026-09-25
scope: [sub-kernel-territory, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, spec-territory, sub-kernel-stalk, sub-libthyla-rs, sub-haul, sub-viv, sub-kernel-devsrv]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 5}
findings: [fnd-b1d-v-r2-f1]
round-of: chg-2026-09-25-b1d-v-emount
created: 2026-09-25
---
## Scope

WIP 2 to WIP 4 (9ce58f0a..e448e2f1): round 1's fixes, the only-`MREPL`-at-a-file
vote, and the documented trailing-slash `-1`. Opus 5.5 at max, the fallback
tier again (Fable out of credits): a context-independent read sharing the
implementer's family, told to re-derive every load-bearing claim from the code.

## Convergence

0 P0 / 0 P1 / 1 P2 / 5 P3. F1 ([[fnd-b1d-v-r2-f1]]) predates the chunk: devsrv
gave every `/srv/<name>` node the registry root's `qid.path`, so the key the new
refusal relies on could name two things. F2: the prose called the install-time
check an invariant, though a 9P server the caller attached can re-type a point
after the mount. F3: `ERRORS.md` counted five `ENOTDIR` producers of eight.
F4: the superseded decision still read as standing. F5: a buggy cfg ran under
symmetry reduction. F6: five imprecise doc comments, one of which ("a kernel
test over every refused flag set") was made true by extending the test to all
24 refused sets. Verified sound: the check over all 32 admitted flag
combinations at both point types, the refcounts, the spec split (22 cfgs), the
kernel tests' discrimination, alloc-smoke's legs, the userspace errno handling,
and the census of mount callers. Round 3 audits the fixes, since the devsrv
change is on a trigger surface of its own.
