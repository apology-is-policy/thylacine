---
id: chg-2026-09-05-crate-extraction-sweep
type: chg
title: "The crate extractions -- the refactor the 2026-08-04 sweep named finally landed"
date: 2026-09-05
arc: arc-vault
commits: ["553d5dd7"]
touched:
  - sub-lib-vt
  - sub-beacon
  - sub-aurora
  - sub-coreutils-lib
  - moc-userspace-shell-tui
  - moc-userspace-runtime
  - moc-userspace-tools
established:
  - sub-lib-vt
  - sub-beacon
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The first absorption on main after [[dec-2026-08-15-cutover]] landed. Two
H-arc crate extractions that were UNOWNED get dossiers, and the two reframes
their extraction forces land with them.

**THE HEADLINE: THE FIX THE 2026-08-04 SWEEP NAMED LANDED.**
[[chg-2026-08-04-presentation-stack-sweep]]'s F1 recorded eighteen aurora
tests that could not compile -- nine of them the interpreter's, over "the byte
machine that eats every byte any program writes to the console", two of those
nine named security regressions that had never executed -- and closed with:
"the refactor that would fix it is the one its siblings already name." That
refactor is H-2a. Aurora's `vt.rs` was extracted to [[sub-lib-vt]], a pure
no_std + alloc zero-dep crate, and the parser that could not compile trapped
inside aurora's unconditionally-no_std crate now carries ~46 host tests
(`cargo test -p vt`), the escape-laundering and out-of-bounds-erase
regressions among them. The prediction was exact and the extraction *was* the
fix -- not a new harness, a new home.

**[[sub-lib-vt]] (usr/lib/vt).** The VT interpreter core: the byte state
machine, deferred autowrap (the nora #37 cascade), the answered
cursor-position report (Kaua's size handshake), the twice-allowlisted OSC 7770
settings channel, the KT-1a widening (DECSTBM/DECOM/SU/SD/wide glyphs), the
alt-screen autowrap save/restore (G-5 F5), and the off-by-default KT-1
boundary capture. Shared by aurora (one Vt per surface) and halcyond (one per
raw-VT pane + the transcript's SGR interpreter). `audit: light` -- a pure lib,
no capability, no AUDIT-TRIGGERS row; the format/security surface is
prosecuted in prose, not fabricated as a hard-coverage gap.

**[[sub-beacon]] (usr/lib/beacon).** The H-1 semantic-output markup crate:
`wire` (the OSC 1936 frame grammar, whose P1 strip property -- strip(rich) ==
none -- is the crate's soul), `sink` (the per-tier realization API that holds
P1 by construction), `verbs` (the H-3c rules engine), plus the cells tier
(boxd/color/palette) relocated verbatim from coreutils (2026-09-01). Placed in
runtime, not the tools cluster it began in: it is the system-wide output
protocol now, consumed by the shell, halcyond, and the 51 coreutils bins, so
it joins [[sub-libtapestry]] on the "client protocol over a boundary" rule.
`audit: light`; `verbs.rs` participates in the H-3c "obj verb menu"
audit-trigger surface, but that surface's HARD gate -- the input grab, the
compositor-owned dismiss, the click-to-focus authority -- lives in
[[sub-tapestryd]]. beacon's contribution is the rules engine's two security
properties (the anti-clickjack rc-quote, the #880 internal-strip), prosecuted
where the code is. Marking the crate `hard` would fabricate coverage gaps for
its six non-verbs files, none of which is an AUDIT-TRIGGERS surface.

**THE CENSUS: FIVE UNOWNED CRATES, NOT TWO.** The handoff named vt + beacon.
A full ownership census of usr/lib -- enumerate, do not guess -- found five
UNOWNED crates: vt, beacon, and three more, `cartoon` (the HALCYON.md 13.2
display list + CPU executor, 616 lines), `libhalcyon` (the Halcyon environment
library: Daylight tokens + the H-4b restore planner, 1708 lines), and
`ptyhold` (a small PTY helper, 148 lines). Two are absorbed here; the three
Halcyon graphics libs are queued as batch 2. A negative over a set you did not
enumerate is a guess -- the census is why the queue is now right.

**[[sub-aurora]] reframed.** It described itself as the interpreter; it is now
vt's HOST on the console path -- owning the pixel side (the atlas blit, the
damage-to-present rectangle) and the two-descriptor console role, feeding vt
the drain bytes and draining its reply/settings queues. The "eighteen tests
cannot compile" caveat is rewritten as resolved, with the residual gap named
honestly: aurora's own no_std render modules still cannot host-test, so the
render side is proven by the in-guest E2E battery, not a unit test.

**[[sub-coreutils-lib]] reframed, and it was doubly stale.** The cells
language left for beacon (re-exported here so call sites are unchanged), AND
the crate grew two modules the resync never added to the code list: `path`
(lexical canonicalization, `realpath -m -s`, also serving BEACON.md 12.2's
obj=path ref rule) and `beacon_gate` (the per-bin tier resolver, the
libthyla-rs-touching half beacon leaves to the caller). The old "four pure
modules / fifteen tests" count was wrong on both ends; corrected to five host
tests over the two pure modules (path, size), with the cells-tier tests noted
as beacon's now.

**LEDGER.** Stale dossiers 65 -> 63 (this batch claimed the two unowned
crates). This is the first absorption to commit CLEAN on main: the
[[chg-2026-09-05-r6-grandfather]] fix cleared the retroactive mirror-count
failures and `quaestor render` cleared the code-coverage view, so the
pre-commit hook passes at 0 fail. The three remaining new crates + the 63
stale dossiers are the arc's continuing work.
