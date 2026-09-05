---
id: chg-2026-08-01-substrate-sweep
type: chg
title: "vault sweep: the substrate"
date: 2026-08-01
arc: arc-vault
commits: []
touched:
  - sub-substrate-build
  - sub-substrate-machine
  - sub-substrate-gates
  - sub-substrate-interactive
  - sub-substrate-builders
established: []
closed: []
opened:
  - seam-87-disk-write-proof
  - seam-70-tcg-watchpoint
  - seam-791-smp1-joey
  - seam-expect-channel-close
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Batch 11. The harness read in full (~9.5 kLOC of shell/python plus the
expect library) against the one reference doc it absorbs; five dossiers
under `system/substrate/` -- the first `audit: none` area, and the first
populated `abis/` registry entry.

ADDRESS-SPACE WAS DEFERRED, deliberately. `aux-2` carries an unmerged
`struct AddrSpace` extraction (418688cf, lineage L-1) that moves SEVEN
fields out of `struct Proc`, and its L-3/L-5 make two Procs SHARE one
address space. Every dossier there would rest on a Concurrency premise the
arc exists to falsify, and the field names are already renamed on the branch
that will merge. Sweeping it now would author known-stale prose in the
section that matters most for an audit:hard area -- the batch-10 hazard
walked into on purpose. It sweeps after the merge.

THE HEADLINE FINDING is a new staleness mode: `09-test-harness.md` is a
JANUS document. Its spine is frozen at P1-F -- "Single-threaded by design at
v1.0 (NCPUS = 1 still)", a "Tests catalog (current)" listing FOUR tests, a
banner example reading `tests: 4/4 PASS` and `phase: P1-F`, a "Not yet
implemented" list naming the 10000-iteration leak check and tests for
scheduler / territory / handle table / 9P client, all of which exist. And
grafted onto that spine are two EXCELLENT current sections (the #77/#92
TEST_YIELD_UNTIL treatment and the whole LS-CI half), one of which contains
the sentence `1232/1233 FAIL`.

So the document states the suite size twice, in two sections, as 4 and as
1233. This is not the additive staleness of the previous five batches --
nothing was appended and left. The document was edited REPEATEDLY, with
care, in two places, and the edits never met. The new sections even carry an
explicit "#72 CORRECTION" retracting an earlier claim of their own, so the
habit of self-correction was live the whole time; it simply never looked up.

Three counts for one quantity, in three places that index each other:

    kernel/test/test.c registry      1237   (across 121 test files)
    09-test-harness.md catalog          4
    REFERENCE.md index row              6

None of the three published numbers is right, and the index row additionally
lists as PENDING ("10000-iteration leak check at P1-I") a test named
`phys.leak_10k` that sits four lines below `phys.alloc_smoke` in the very
registry it indexes. The self-similar micro-instance: the LS-CI section's
"Four portability facts are load-bearing" is followed by SIX numbered items.

ALSO CORRECTED -- the vault's own note. `gate-smp` described TWO
classification classes; `tools/smp-multiboot.sh` has FOUR, and the fourth
(OTHER, an unclassified nonzero exit) FAILS THE GATE. The note was written
from the memory-file description of the gate rather than from the script.
Rewritten against the code, with the INJECT-MISS green-guest proof and the
two precision rules (#362's catch-all regex, the bare-`canary` false
positive) it was missing.

Recorded seams: [[seam-87-disk-write-proof]] (a reused disk.img disarms the
virtio-blk write proof -- LS-CI mitigates per-attempt, test.sh and the SMP
gate stay exposed), [[seam-70-tcg-watchpoint]], [[seam-791-smp1-joey]],
[[seam-expect-channel-close]].

New registry notes: [[abi-boot-banner]] (the first `abis/` entry -- the
registry the schema declares and `workflow.md` names, empty until now),
[[gate-interactive]], [[gate-v80-floor]], [[haz-harness-fail-open]] (the
area's organizing hazard, with its eight worked instances split by
direction: read-as-guest is costly, read-as-verified is worse).
