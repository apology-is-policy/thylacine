---
id: fnd-net6b-r1-f5
type: fnd
title: "The lockless p->poll fast-path read was unannotated"
round: adt-net6b-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p-poll]
threatens: []
fixed-by: chg-2026-06-18-net6b4-close
created: 2026-07-31
---
## Prosecution

The double-checked lazy-init's fast-path pointer load had no ordering
annotation against the publish.

## Disposition

Fixed: ACQUIRE load paired with the RELEASE publish. Hygiene +
future-proofing on aarch64 (pointer loads tear-free; the init is
KP_ZERO-equivalent) -- not a live bug, annotated as such in code.
