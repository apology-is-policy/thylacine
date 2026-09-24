---
id: arc-halcyon-interaction
type: arc
title: "Halcyon interaction: pointer, clipboard and modal text"
status: active
design: [docs/HALCYON-INTERACTION.md]
chunks: []
follow-ons: []
exit-criteria:
  - "[ ] Portable guest cursor works over composed/fullscreen content and capture transitions"
  - "[ ] Bounded session clipboard preserves local registers and enforces controller/focus admission"
  - "[ ] Focused context mode appears correctly in the existing Instrument status widget"
  - "[ ] Nora and transcript share navigation, selection, search and explicit clipboard actions"
  - "[ ] Native paste inserts text without submitting a shell command"
  - "[ ] Boosty native text-field integration is verified on the real browser"
  - "[ ] Runtime workflow evidence, screenshots, operator manual and as-built dossiers are complete"
created: 2026-09-24
---
## Goal
Deliver the interaction direction recorded in [[dec-2026-09-24-halcyon-interaction]].
`docs/HALCYON-INTERACTION.md` is the approved specification. HI-0 is starting. Authority work and its saved drafts remain separate.

## Planned chunks
HI-0 pointer; HI-1 controller and service contract/ABI; HI-2 mode widget and Nora
clipboard bridge; HI-3 transcript selection/search and typed paste; HI-4 Boosty
integration with Main; HI-5 qualification and operator documentation.

The transport is the existing session 9P service extended with bounded hand-built
interaction records. No Mycelium dependency or additional per-session /srv slot.
The new document identifies current paneplace connection limits as a design
constraint to resolve before persistent application clients are added.

## Close summary
Open. Full specification approved for implementation on 2026-09-24.
No code, runtime verification or new UI captures belong to this arc yet.
