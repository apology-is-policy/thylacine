---
id: arc-corvus-srv
type: arc
title: "P5 corvus + /srv (the key agent and its kernel channel)"
status: active
design: ["docs/CORVUS-DESIGN.md"]
chunks:
  - chg-2026-05-19-srv-birth
follow-ons: [seam-srv-registry-lifecycle]
created: 2026-07-31
---
## Goal

Phase 5's corvus bring-up: the `/srv` kernel mechanism (registry +
per-connection transport + accept + kernel-stamped peer identity) and
the corvus userspace key agent served over it. Historically finished
(the arc closed with the P5-corvus-srv-impl audit, 2026-05-20); the
chunk list above is the srv-area sweep's kernel-side backfill and stays
partial until the corvus-userspace sweep adds its half — the arc flips
to `complete` then.

## Close summary

(written at status flip to complete)
