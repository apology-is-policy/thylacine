---
id: fnd-66b-r1-f3
type: fnd
title: "The read-buffer headroom comment assumed short names"
round: adt-66b-r1
severity: P3
status: fixed
surface: [sub-kernel-territory]
threatens: []
fixed-by: chg-2026-06-12-66b-mp-path
regression: "none (documentation)"
created: 2026-08-01
---
## Prosecution

The `DEVPROC_READ_BUF` comment justified 512 bytes as "~16 B/line,
comfortable headroom" — a per-line estimate drawn from the boot
namespace's short names (`/srv`, `/proc`, `/dev`). But a mount point or
source name can be up to `SYS_OPEN_PATH_MAX` = 1024 bytes, so a SINGLE
deep name can exceed the whole buffer, and truncation can arrive at
entry one rather than after a dozen.

The sizing was fine; the STATED REASON for it was wrong in a way that
would mislead the next person sizing it.

## Disposition

Fixed as documentation: reworded to say 512 bytes holds the common
short-name boot layout, that deep names truncate cleanly at a whole-line
boundary (per [[fnd-66b-r1-f2]]), and that completeness for deep
namespaces needs an offset-aware multi-read.

A headroom argument that averages over the expected case says nothing
about the worst case — and the worst case here is a single entry, not an
accumulation.
