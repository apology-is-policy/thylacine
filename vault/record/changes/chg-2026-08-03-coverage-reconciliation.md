---
id: chg-2026-08-03-coverage-reconciliation
type: chg
title: "the coverage reconciliation -- the sweep is 31%, and the completeness criterion was the defect this arc keeps finding"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - view-code-coverage
  - arc-vault
established:
  - view-code-coverage
closed: []
opened: []
supersedes: chg-2026-08-02-server-sweeps
depth: skeletal
created: 2026-08-03
---
Batch 28. The plan said absorb the twelve prose-swept documents. Choosing which
twelve required knowing that each one's replacement notes exist, and asking that
question produced a different batch.

**THE CLAIM WAS FALSE AND I MADE IT.** [[chg-2026-08-02-server-sweeps]] closed
with *"the subsystem sweep is COMPLETE -- every kernel subsystem and every
`usr/` service now has a dossier."* The census says **133 of 421 source files
are owned by a dossier: 31%.** Unowned: `mmu.c` (1921 lines, which is where
W^X lives), `notes.c` (1043), `fault.c` (928), `exec.c` (704), `hwdebug.c`
(523), `pts.c` (475), `vma.c`, `spoor.c`, `elf.c`, `image.c`, `halls.c`,
`dev.c`, and nearly all of userspace.

Not partially covered. `fault.c`, `exec.c`, `pts.c` and `vma.c` appear in
**zero** system notes -- I checked, because a census that counts a file unowned
when a dossier merely forgot to list it would be making the opposite error.

**THE CRITERION WAS STRUCTURAL WHERE IT NEEDED TO BE EXTENSIONAL.** Completeness
was read off the area MOCs: every area has children, every planned area is
swept, therefore done. The question was whether every *file* has an owner. The
MOC tree answers "did I visit each area", and I read it as "did I cover each
area", and those differ by exactly the files an area sweep did not reach.

Which is this arc's own finding, turned inward. Batch 24: the assertions pin the
values, nothing pins their description. Batch 25: the models pin the mechanisms,
nothing pins the model's scope. Batch 26: each copy is pinned to itself, nothing
pins the copies to each other. Batch 27: the guard travelled, the reason did
not. Batch 28: **the ledger pins the areas, and nothing pins the areas to the
tree.** A claim whose subject is narrower than the claim -- found four times in
the code, and the fifth time in the bookkeeping that found them.

**AND IT FALSIFIES THE STANDING STORY.** For twenty batches the account was that
the sweep ran ahead while absorption lagged; that is what [[view-absorption]]
was built to measure. If it were true, code ownership would be near total with
absorption at 46/147. Absorption is 31%. **Ownership is 32%.** The two ledgers,
measuring different things by different methods, agree -- so the sweep was never
ahead. It tracked absorption the whole way, and the gap I was correcting at
batch 23 was not between two passes but between both passes and the tree.

**THE KERNEL IS 67% OWNED; USERSPACE IS NOT.** That is the honest split, and it
is what makes the false claim believable in hindsight -- the kernel sweep really
did work, and the areas that came last were userspace, where "every `usr/`
service has a dossier" was true of netd, ptyfs, tapestryd, login and joey and
false of corvus, warden, aurora, diorama, prowl, utopia and nora. `libthyla-rs`
alone is 29 files and 13k lines, unowned, and is the substrate every native
program is built on.

**THE FIX IS THE ONE BATCH 23 ALREADY WROTE DOWN.** [[view-code-coverage]]
computes ownership from `git ls-files` against every `sub` note's `code:` field.
Prose mention does not count -- a dossier that discusses a neighbour's file has
not swept it, and counting mentions is how the first ledger rotted. That makes
it pessimistic in the safe direction: it can call a swept file unowned (fix the
frontmatter), never call an unswept file covered. Deterministic across renders,
so a drift shows up as a stale body rather than a flapping number.

PROBE, and it went badly in the instructive direction. **P1** -- make a dossier
claim `arch/arm64/mmu.c`, a real file it has never swept. The entry passes,
`mmu.c` moves from unowned to owned, and the only failure is a stale generated
body: the count moved and the view had not been re-rendered. Re-render, and the
corpus is clean with a false claim in it. So the new ledger can be inflated by
exactly the mechanism that produced the claim it was built to correct -- **an
assertion nobody checks.** It is a smaller hole (a `code:` entry is a deliberate
statement about a named file; "the area has children" was an accident of
structure) and it is a real property that a false claim landed *without* a
re-render is caught. It is not a closed hole, and the view now says so with the
probe in it, replacing my own sentence -- written an hour earlier -- claiming
the number could "never" call an unswept file covered.

**P2** -- name a path that does not exist. Also passed, and that one is
resolvable, so by this arc's own rule it should not have. Fixed in the probe:
`checkCodePaths` now fails a `code:` entry with no file behind it, including
`<repo>: path` entries against the sibling Stratum tree when it is checked out
(all eight existing ones verified; skipped rather than failed when the sibling
is absent). Re-run: caught, and nothing else in the corpus fails. The useful
case is not fabrication but rot -- a rename or delete that would otherwise leave
a dossier owning nothing while the ledger kept counting it.

The view also names the blind spot it cannot close: whether a dossier that
genuinely covers a file *still* covers it after the file changes. That one is
computable from git and is owed (task #38).

**THE HARNESS EXCLUSION IS DECLARED, NOT APPLIED SILENTLY.** 59 files and ~22.5k
lines of probes, smokes, benches and the `u-test` family are excluded, on the
line already drawn for `tools/`: a program whose purpose is to exercise the
system is harness. Drawing that line while reporting my own coverage is exactly
where a denominator gets quietly narrowed, so the excluded count is printed in
the view's headline rather than dropped.

**WHAT THIS BATCH DELIBERATELY DID NOT DO.** No document was absorbed. Absorbing
while the underlying sweep is a third done accelerates toward a cutover that
should not happen yet -- the point of the cutover is that CLAUDE.md can point at
the vault instead of the reference, and pointing it at an absent dossier is
worse than pointing it at a stale document. The absorption criterion is
rewritten to be computable (a document is absorbable when every file it
documents is owned), and the cutover criterion now says explicitly that it gates
on the sweep.

The real remaining work is enumerated rather than estimated: tasks **#50-#57**,
roughly eight areas, of which four hold section-28 invariants (I-12 in the
mapping core, I-19 in note delivery, I-36 in exec, I-20's stop leg in `pts.c`).

**A CORRECTION THAT POINTS ONLY ONE WAY.** The Record plane is append-only and
corrections are new notes with `supersedes` (schema R3), which this note
carries. But `chg.superseded-by` is not in the closure set -- `validate.go`
permits `superseded-by` on a `dec` only. So this note points at the false claim
and the false claim cannot point back: a reader landing on
[[chg-2026-08-02-server-sweeps]] reads "COMPLETE" with nothing to warn them.
The correction is discoverable from the correcting side alone. Task #58, and
the same shape as everything above -- a link that exists in one direction reads,
to whoever is standing at the other end, exactly like no link at all.

LEDGER. Corpus 796 -> **798**. Absorption unchanged at 46/101/147 -- deliberately.
Code coverage **133 owned / 288 unowned / 421 files (31%)**, plus 59 harness
files excluded and counted. Spec coverage unchanged at 33/33, which remains
true: that view censuses `specs/` against spec notes, and every module does have
one.
