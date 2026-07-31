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
  - chg-2026-06-23-gonet3c-net-over-net
  - chg-2026-06-24-349-flow-control
  - chg-2026-06-24-348-s2c-blocking
  - chg-2026-07-08-cf3b-bulk-ring
  - chg-2026-07-09-larder-l1c
  - chg-2026-07-09-larder-l1d
  - chg-2026-07-09-larder-l1e
  - chg-2026-07-09-larder-l1f
  - chg-2026-07-11-wb-staging
  - chg-2026-07-11-fid-lifecycle
  - chg-2026-07-11-b1-loose
  - chg-2026-07-11-d44-read-band
  - chg-2026-07-12-term2-dentry-name
  - chg-2026-07-13-375-spill
  - chg-2026-07-13-5253-send-dispositions
  - chg-2026-07-13-g1-write-populate
  - chg-2026-07-13-g2-dirfid
  - chg-2026-07-13-g34-downgrade-genscope
  - chg-2026-07-14-term4-close
  - chg-2026-07-19-99-create-errno
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
