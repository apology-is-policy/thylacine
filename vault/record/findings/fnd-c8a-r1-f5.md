---
id: fnd-c8a-r1-f5
type: fnd
round: adt-c8a-r1
severity: P3
status: fixed
title: "torpor.wake_counts_only_a_sleeper leaked on a failure"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-03-c8a-r1
regression: "none: a test fixture"
created: 2026-11-03
---
## Prosecution

An early assert left its child and sharer behind.

## Disposition

Every step is bounded and recorded, with the asserts after the release.
