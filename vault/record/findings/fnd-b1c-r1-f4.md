---
id: fnd-b1c-r1-f4
type: fnd
title: "/heap-probe's trim leg could not fail: the frees had already trimmed, so it passed a trim() that did nothing"
round: adt-b1c-r1
severity: P2
status: fixed
surface: [sub-thyla-heap]
threatens: [inv-i32]
fixed-by: chg-2026-09-24-b1c-round1-close
regression: "heap-probe trim-premise-kept (>= 192 pages held before the trim) and trim-returns-the-rest (<= 32 over the base after it); the trim sabotaged to a no-op on the device RED by name (211 pages against <= 32)"
created: 2026-09-24
---
## Prosecution

`trim-returns-the-rest` read 14 pages over the base on all three boots of run4,
the figure `small-blocks-fall` had reported before it. The frees had coalesced
into a top past dlmalloc's 2 MiB trim threshold and trimmed it themselves, so the
explicit `trim()` found nothing to return, and the leg would pass a `trim()` that
did nothing. The round rated it P3; the main session's self-audit, which found
it independently as S-A1, rated it P2, and the higher governs.

## Disposition

Fixed. The leg builds its own premise: sixteen blocks of 64 KiB, touched and
freed, leave a top of about 1 MiB, under the threshold, so no free trims it. It
checks that the pages are still held (at least 192), then that `trim()` returns
them (at most 32 over the base), and a patterned block below the trimmed region
must stay intact. On the device, the trim sabotaged to a no-op failed the leg by
name.
