---
id: arc-halcyon-interaction
type: arc
title: "Halcyon interaction: pointer, clipboard and modal text"
status: active
design: [docs/HALCYON-INTERACTION.md]
chunks: [chg-2026-09-24-hi0-pointer, chg-2026-09-24-hin1-envelope]
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

## Implementation checkpoint, September 24

HI-0 now has source for the standard pointer shapes, private Lictor cursor
plane and trusted exclusion, plus the capture harness. HI-1 has an envelope
foundation and C mirror. The Pi host suite passes 274 tests. Source remains
uncommitted while boot-image and actual UI qualification wait on the coordinated
Mac lease; the earlier close-summary sentence describes ratification time.
No new UI screenshot or completed clipboard workflow is claimed yet.

## Runtime checkpoint, September 24, 16:18 UTC

The heartbeat acquired Mac, completed Main reconciliation through `5857b6bf`
as `08c26509`, built the CI image and released Mac. The separate authority
drafts match their saved stash bytes. Under a Pi lease, the isolated QEMU/KVM
runs pass the full pointer scenario with 2D and VirGL 3D composition. Real VNC
framebuffer/cursor-plane captures verify the visible shapes and transparent
SAK exclusion; guest ctl provides their acknowledged positions. The broader
graphical SAK state/recovery scenarios are still running. Clipboard service,
application capture, direct scanout and wider qualification remain open.

### Completed queued runtime checks

The three accelerated SAK scenarios subsequently passed, including real expiry,
lockout and recovery. Refreshed current-source host tests pass 274/274 and the
separate boot/probe gate passes. Its skipped production/external-fixture rows
remain unqualified. The heartbeat's resource wait succeeded; the larger
interaction implementation remains open beyond this pointer checkpoint.

The final pointer harness, including the embedded raw-pixel assertions, also
passed on VirGL. Pi was released with no VM left running and the queue heartbeat
was paused after completing its requested runtime verification. Captures and
remaining obligations are indexed by `docs/HALCYON-INTERACTION-STATUS.md`.
