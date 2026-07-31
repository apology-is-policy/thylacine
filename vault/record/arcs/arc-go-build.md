---
id: arc-go-build
type: arc
title: "The on-device Go build: enablement, then clean + perf"
status: active
design:
  - "docs/CONCURRENT-FS.md"
  - "docs/LARDER-DESIGN.md"
  - "docs/FID-LIFECYCLE-DESIGN.md"
chunks:
  - chg-2026-06-24-349-flow-control
  - chg-2026-07-13-375-spill
  - chg-2026-07-13-5253-send-dispositions
follow-ons: []
created: 2026-07-31
---
## Goal

Make the on-device `go build` work, then clean (every finding chased to
ground -- the build as a whole-kernel stress oracle) and fast (the
warm/cold-floor decomposition). The 9P-client chunks here are the slice of
that mission that landed on the shared client.

## Planned chunks

HISTORICALLY (SUBSTANTIALLY) COMPLETE. Held `active` while the Record
backfill accretes this era's chunks; the list above is the vault-backfilled
subset. The mission register lives in the harness memory
(project_go_build_clean_perf) until the sweep absorbs it.

## Close summary

(written at status flip to complete)
