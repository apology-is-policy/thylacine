---
id: arc-go-ide
type: arc
title: "The Go IDE + cross-boundary debugger (Ambush)"
status: active
design: ["docs/GO-IDE-DESIGN.md", "docs/DEBUG-FS-DESIGN.md"]
chunks:
  - chg-2026-07-17-8c3-reader-role
  - chg-2026-07-19-90-death-block-through
follow-ons: []
created: 2026-07-31
---
## Goal

The on-device Go IDE stack: the kernel debug surface (I-39), the Ambush
debugger, nora integration. The 9P-client chunks here are the debug-stop /
death interactions with the shared elected reader.

## Planned chunks

GENUINELY ACTIVE (ambush single-step-`next` remains open). The list above
is the vault-backfilled subset of its landed chunks.

## Close summary

(written at status flip to complete)
