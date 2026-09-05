---
id: seam-console-chrome-on-handoff
type: seam
title: "a console renderer's DISPLAY-level chrome (status bar, placed menu) is not backgrounded with it on the session handoff"
status: open
surface: [sub-tapestryd]
opened-by: fnd-kt1-r1-c11
tracker: "the KT-1 audit round 1 (C-F11)"
created: 2026-09-05
updated: 2026-09-05
---
## Owed

[[fnd-kt1-r1-c11]]: while a declared session holds the display, a renderer-registered status bar still forces the composed path and stays painted over the session, and a menu placed at the handoff keeps the input grab. aurora registers neither, so the shipping image cannot reach it; the retired halcyond-console lever can.

## What closes it

Either the handoff dismisses/hides the renderer's display chrome (the compositor drops the carve and the grab while a declared session hosts a leaf) or the renderer is required to do so on its backgrounding FOCUS/FRAME edge; a gate that registers a status bar under a session and asserts the session's Direct scanout.

## Risk while open

None on the shipping image (aurora has no chrome). A renderer with chrome would degrade the session to composed with a strip painted over it, and a placed menu would swallow the session's input.
