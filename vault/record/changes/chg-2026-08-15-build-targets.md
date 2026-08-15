---
id: chg-2026-08-15-build-targets
type: chg
title: "build.sh re-swept: three target lists that disagree, and the vault recommended the shortest"
date: 2026-08-15
arc: arc-vault
commits: ["*(pending)*"]
touched: [sub-substrate-build, sub-pouch-seam]
established: []
closed: []
opened: []
mirrors-checked: [tools/build.sh, tools/check-patch-hunks.py]
depth: rich
created: 2026-08-15
---
Twenty commits on `tools/build.sh` since the sweep, ~841 lines. The target set
nearly doubled — the compiler toolchain, the graphics stack and three ported
programs all arrived — and two new guards landed with it.

## The finding, and it was in this dossier's own advice

`tools/build.sh` names its target set **three times and no two agree**:

| where | count |
|---|---|
| the `case` dispatcher — ground truth | **19** |
| the "Unknown target" error's `Valid:` list | 15 |
| the header comment block | **10** |

Nothing is advertised that does not exist, so there is no phantom; the drift is
one-directional, toward silence. The whole compiler-toolchain group is invisible
in both short lists.

**And [[sub-substrate-build]] told readers to prefer the header** — *"the most
accurate documentation of the target chain in the tree and actively maintained
— prefer it to any prose elsewhere, including the absorbed reference docs."*
That was true when written and is now the worst of the three.

The decay mechanism is the interesting part. A target added to the dispatcher
works perfectly whether or not the header mentions it: no build error, no test,
no complaint — because the people adding targets already know they exist. The
only reader who pays is the one who does not, and to them an omission is
indistinguishable from an absence. Which is exactly why the *recommendation* was
the dangerous half: it routed that reader to the list most likely to be short.
Task #180.

## What is genuinely new, and one of it is the file's best structure

- **A patch-hunk check at one unconditional chokepoint, ahead of the
  dispatcher.** It validates unified-diff hunk line counts across the whole
  hand-written series — ~50 ms for 281 hunks — and its comment gives the
  reason: there are several `patch` loops, so verify at *one* chokepoint rather
  than copying a check into every caller. It exists because the tool ate a
  function definition out of a port patch **and exited zero**.

  Different in kind from the two staleness checks: those watch mtimes and can
  only warn; this one reads the artifact's own internal arithmetic and refuses.

- **A stale-stage warning** for a staging step the main chain never refreshes,
  so the pool re-mints faithfully around the *previous* binary with every ledger
  line green. Paid for twice; the second time a gate failed 3/3 on a binary 27
  minutes older than the fix under test, *looking exactly like a real defect in
  the change*.

## The second finding, filed at the strength the evidence supports

That warning's comment says it is *"checked for EVERY staged GL binary … a
name-by-name check would go quiet again the moment a binary is added — which
tyr-glquake then was."* The loop two lines below is a name-by-name check of
four.

**My first instinct was "the fifth will go quiet", and that would have been
wrong.** Diffing the staged set against the watched set found them identical,
nothing staged-but-unwatched. The check is complete today.

What survives is weaker and still real: the set is maintained by hand in two
places, the comment claims a property the code achieves by coincidence of
maintenance, and the failure mode is *a warning that does not print*. One line
from safe-by-default, because the staging step already computes the
authoritative set. Task #181.

## A narrowing

[[sub-pouch-seam]] claimed `tools/build.sh` in its `code:` list and mentioned
the file exactly once — in that claim. The claim dated from the original
landing ("0001 + 0002 + the build wiring") and had never been paid for, while
[[sub-substrate-build]] describes the sysroot rebuild, the patch application and
the staleness checks in full.

Dropped. What it cost while it stood was a false signal: an 841-line churn
figure attributed to a dossier describing none of it, competing for a place at
the head of the sweep queue. The batch-35 precedent, restated — traversal is not
a sweep, and neither is being *built by* something.
