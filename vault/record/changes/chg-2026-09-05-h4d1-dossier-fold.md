---
id: chg-2026-09-05-h4d1-dossier-fold
type: chg
title: "H-4d-1 folded into the dossiers: the creator reservation (sub-tapestryd) + the println one-write form (sub-libthyla-rs)"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-tapestryd
  - sub-libthyla-rs
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
main's H-4d-1 (`c96f5173`, [[chg-2026-09-05-h4d1-creator-reservation]]) touched
sub-tapestryd and sub-libthyla-rs; its chg carried `no-dossier-change` naming
both deltas and deferring the prose to the vault peer (the KT-1 inheritance
pattern). This is that fold, verified against the landed code.

## sub-tapestryd -- the creator reservation

The claim race H-4c left owed: under a session the compositor is the user's rio
and fills every empty leaf it owns, while the restore tool -- the SAME principal
-- is mid-build; owner-gating the claim (H-4b-2) does not separate them. H-4d-1
marks the leaf AT the split with `Pane.creator_conn` (verified pane.rs 273,
stamped 433, released 447-448), stamped on BOTH empties by a ctl split (a chord
split stamps none). The claim mint answers **E_AGAIN** -- not E_PERM -- to any
other conn of the same principal while the creator lives (verified server.rs
13808: `creator != 0 && creator != self.conn_id && !Renderer` -> E_AGAIN, "the
leaf IS its principal's, just not yet"); the Renderer is never held off; the
release rides `retire_conn` and fans one TEV_LAYOUT. Folded as a new subsection
after the owner-gated-claim paragraph in the Session-actor section. The paired
session-tile-hosting decision (the tag IS the command line) is halcyon's
(docs/reference/150/151, unowned per main's record); the tapestryd half is the
reservation + the menu authority checks' Session(p) arms.

## sub-libthyla-rs -- the println one-write form

`print!`/`println!`/`eprintln!` now format the whole line (and its trailing
newline) into one buffer and issue ONE `SYS_PUTS`, rather than one syscall per
format fragment (verified lib.rs 3387/3396: `Stdout/Stderr::write_fmt`, "the line
AND its newline in one write"). Not a performance tidy: the console's writer role
is claimed per write, so the fragment-per-syscall form let a concurrent writer
interleave mid-line (the torn line the session gate's rc leg hit). Folded into
the existing `print!`-family Error-paths note.

## Not a re-derivation

The gesture's narrative, the placement rules, and the audit (it rides
AUDIT-TRIGGERS' H-4b row, unaudited by the double-the-distance rule) are main's
record's; this only brings the two vault-owned dossiers current with the landed
code. halcyond/halcyon are unowned (150/151 carry their prose).
