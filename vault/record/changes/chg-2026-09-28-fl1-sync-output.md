---
id: chg-2026-09-28-fl1-sync-output
type: chg
title: "FL-1: a synchronized frame (DEC ?2026) holds the paint -- the vt tracks and reports the mode, the kaua seam carries it as sync_begin/sync_end, halcyond and aurora hold the paint, and lantern writes one frame per slide"
date: 2026-09-28
arc: arc-tapestry
commits: ["SQUASH"]
touched:
  - sub-lib-vt
  - sub-kaua-term
  - sub-halcyond
  - sub-aurora
  - sub-lantern
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-28
---
The operator saw a lantern slide change flicker over Haul and voted the seam
record ([[dec-2026-09-28-sync-output-seam]]): a program's synchronized frame,
DEC private mode 2026, crosses the kaua seam as `Control::SyncBegin` and
`SyncEnd` (subtags 7 and 8), and halcyond holds only that tile's paint until
the close, a reconfigure, the program's exit, or 150 ms after the first paint
it deferred. The vt reads a CSI's marks now -- a private marker on the first
byte only, intermediates, C0 in place -- so a marked sequence no longer runs a
plain handler or prints its tail; it answers DECRQM for the DEC modes it
tracks; and it reports each change of mode 2026 in stream order, RIS closing
an open frame last ([[sub-lib-vt]]). `FrameHold`, shared by both renderers,
bounds the wait: the bound survives a close and a reopen, a timeout abandons
the frame, and a dead or backwards clock never holds. The producer flushes the
pending cells before each record ([[sub-kaua-term]]); halcyond's render step
skips a held tile's paint, the loop waits in the poll for the rest of the
frame instead of looping back for it, the poll deadline wakes it at the
bound, a reconfigure or the program's exit cuts the hold, and a test-mode
line says how a held frame ended -- only the program's own close reads as
shown whole ([[sub-halcyond]]); aurora reads
the mode and a count of frames opened once per pass ([[sub-aurora]]); lantern
reads and renders a slide before it writes, then writes the bracketed frame
once ([[sub-lantern]]). Audited by holotype-reviewer, Fable 5.1 reviewing
Opus 5.5: round 1 found one P0 -- the session loop spun on a held tile, which
the device leg had found first -- and three P3s; round 2, on the fixes, four
P3s; closed. The device leg (`ls-halcyon-lantern` 8) shows a slide change held
as one frame, and fails without lantern's marks, with the spin, and with a
render step that counts the hold but paints anyway.
