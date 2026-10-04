---
id: chg-2026-10-04-hidden-storage-implementation
type: chg
title: "Retire hidden terminal pixels while preserving live panes"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-tapestryd, sub-libtapestry, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The approved storage contract now has compositor, client library and terminal
adoption. Hidden buffers retire cooperatively; fresh generation-bound fids and
full repaint restore the same semantic pane. Jobs and transcripts survive.
The128MiB shared-map ceiling and all other quotas are unchanged.

Native original full-width16PTY/14controller+F10 passes107.63s. Protocol36.67s
includes stale offers/fids, pinned nonaliasing old maps and real map refusal
followed by abort/release/recovery. Hidden output/resize/close60.29s and existing
media/manual/theme/SAK71.60s have inspected1280x800 captures.122+23host tests,
11source mutants and the model-first clean/mutant gates pass; boot1830/1830.

Single-agent WIP review, not a Main landing or broad qualification. No fresh
SMP/sanitizer/Pi/minimum-display claim. Failed-reveal notice capture and the
clipboard/modal application work remain open. Evidence: work/oct4-hidden-storage.
