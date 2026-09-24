---
id: dec-2026-09-24-halcyon-interaction
type: dec
title: "Halcyon interaction: shared modal vocabulary, session clipboard and native pointer"
date: 2026-09-24
status: standing
decided-by: user-vote
affects: [sub-halcyond, sub-tapestryd, sub-nora-engine, sub-nora-host, sub-libhalcyon]
created: 2026-09-24
---
## Fork
The operator requested Nora-style transcript navigation, search and selection,
a shared clipboard, a focused-tile mode widget and a visible native pointer.
Should modal state be intercepted globally, and should the IPC wait for Mycelium?

## Research
The inspected tree has transcript INS/NOR and local yank, Nora's separate mode
machine and local register, routed pointer input and the session paneplace 9P
service. Mycelium remains a planned native IPC consolidation, not an implemented
prerequisite. Helix's explicit shared-clipboard bindings provided the proposed
local-versus-shared distinction. Details and source anchors are in
`docs/HALCYON-INTERACTION.md`.

## Options
One global mode machine versus application-owned modes with shared actions and
reporting; clipboard-first y/p versus local y/p and Space y/p for sharing; wait
for Mycelium versus a small service on current 9P machinery.

## The call
The operator agreed to the proposed direction: one input owner, focused-mode
reporting, Helix-style local/shared register separation, and the native pointer.
The operator explicitly chose to hand-build the 9P service for this work and
potentially convert it to Mycelium later, then requested a specification.

After receiving the complete specification, the operator said, "I love it.
Let's start." The detailed specification is approved for implementation.
Numeric wire layouts are pinned in HI-1 before consumers; no implementation
or runtime result is claimed by this design decision.

## Rationale
Keep Nora's editing behavior, make cross-application text transfer predictable,
and give Boosty a visible pointer and native text-field integration. The clipboard
and mode service become concrete Mycelium consumers without making their delivery
wait for the separate IPC consolidation arc. See [[arc-halcyon-interaction]].
