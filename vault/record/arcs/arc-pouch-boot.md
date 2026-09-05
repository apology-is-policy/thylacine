---
id: arc-pouch-boot
type: arc
title: "P6 pouch + stratumd boot (the ported-userspace bring-up)"
status: active
design: ["docs/POUCH-DESIGN.md"]
chunks:
  - chg-2026-05-26-16c-attach-srv
  - chg-2026-05-23-torpor
  - chg-2026-05-26-16bg-hardening-f3f4f5
  - chg-2026-05-25-16b-beta-hw-openat
  - chg-2026-05-25-16b-gamma-syscalls
  - chg-2026-05-25-16b-gamma-mount-close
  - chg-2026-05-26-16bg-hardening-3b
  - chg-2026-05-26-16bg-hardening-3c
  - chg-2026-05-26-16c-pre
follow-ons: [seam-848-pivot-walk-race]
created: 2026-07-31
---
## Goal

Phase 6's ported-userspace bring-up: the pouch (musl boundary-line),
stratumd as the on-device FS server, and the boot path that mounts the
disk-backed root (16a..16c). The 16c chunk -- the srvconn transport +
SYS_ATTACH_9P_SRV + SYS_PIVOT_ROOT -- is the slice this sweep absorbed.

## Planned chunks

HISTORICALLY COMPLETE (May 2026). Held `active` while the Record backfill
accretes this era's chunks; the list above is the vault-backfilled subset.
The full record: docs/POUCH-DESIGN.md section 14 + phase6-status + git log.

## Close summary

(written at status flip to complete)
