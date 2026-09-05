---
id: chg-2026-09-05-halcyon-libs-sweep
type: chg
title: "The Halcyon graphics libs -- the three crates the handoff did not name"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-cartoon
  - sub-libhalcyon
  - sub-ptyhold
  - moc-userspace-runtime
established:
  - sub-cartoon
  - sub-libhalcyon
  - sub-ptyhold
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
Batch 2 of the crate-extraction sweep: the three UNOWNED usr/lib crates the
[[chg-2026-09-05-crate-extraction-sweep|batch-1]] census turned up that the
handoff had not named. With these the census is closed -- five unowned crates
found, five dossiers written.

**[[sub-cartoon]] (usr/lib/cartoon).** The Halcyon display list + its dumb CPU
executor (HALCYON.md 13.2). halcyond -- the only place that thinks -- draws a
`Cartoon`; the executor weaves it to pixels and knows nothing (no shaping, no
measuring, no diff). The dossier's spine is the two safety properties that
make a dumb executor trustworthy: every write is fully clamped so no op can
reach outside the pixel buffer whatever the list says, and a `Glyphs` op paints
only against the atlas generation it was authored for, making a stale page
reference impossible by construction. The blend is the shift form -- the exact
scar [[sub-aurora]] records, where a divide over a packed word corrupted
antialiased edges. audit: light (pure lib, no capability, no AUDIT-TRIGGERS
row).

**[[sub-libhalcyon]] (usr/lib/libhalcyon).** The Halcyon environment library:
`theme` (the Daylight visual scripture as code -- the SINGLE token source the
H-3 split names, so halcyond and [[sub-tapestryd]] read the same colours from
here and nowhere else), `layout` (the `halcyon-layout v1` save format, an
untrusted-`$home`-input parser that is bounded, fail-closed, and no-panic), and
`skeleton` (the pure restore planner, a MODEL of the compositor's split
rule). Its only dependency is [[sub-lib-vt]] (theme returns `vt::Palette`).
audit: light; `layout.rs` + `skeleton.rs` participate in the H-4b
"Session(principal)" audit-trigger surface, but that surface's HARD gate -- the
Session actor, the claim, PFK_OWNER -- lives in [[sub-tapestryd]]; libhalcyon
holds the authority-free planner half. Same shape as beacon/verbs at H-3c.

**[[sub-ptyhold]] (usr/lib/ptyhold).** The shared PTY master-hold core
(mint/seed/spawn), extracted verbatim from `/bin/ptyhost` at PTY-4 so the
kaua-term reuses it. The dossier's spine is the fd-ownership contract the
`HoldError` enum encodes exactly -- which failures leave the master fd open
(caller owns it) versus opened-then-closed -- and the drain-then-EOF arming
that depends on `spawn_on_slave` retaining no parent copy of the slave. audit:
light (a delicate fd-lifetime surface, but no capability and no AUDIT-TRIGGERS
row; the one crate this batch that depends on [[sub-libthyla-rs]] and so is not
host-testable).

**PLACEMENT.** All three join runtime beside [[sub-libtapestry]], [[sub-beacon]],
and [[sub-lib-vt]] -- they are shared libraries over a boundary, not
shell/TUI apps. cartoon and libhalcyon are Halcyon graphics libs; ptyhold is a
terminal-host mechanism whose consumers (kaua-term, ptyhost) are terminal
hosts, but it is a mechanism library, not an app.

**LEDGER.** Coverage rises by the three crates' files; the stale-dossier count
is unchanged (these were UNOWNED, not stale -- claiming unowned code raises
coverage without touching the stale set). lint 0 fail. The 63 stale dossiers
and the queued DX-8 tooling absorption (gated on aux-3 reaching main --
1ae31536 is on aux-3 only) are the arc's continuing work.
