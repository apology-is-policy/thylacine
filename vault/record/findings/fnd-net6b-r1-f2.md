---
id: fnd-net6b-r1-f2
type: fnd
title: "OOM with no covering op parks an infinite-timeout poll unwakeably"
round: adt-net6b-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p-poll]
threatens: [inv-i9]
fixed-by: chg-2026-06-18-net6b4-close
created: 2026-07-31
---
## Prosecution

A cand_op allocation failure with no live op to wake the registered hook
leaves an infinite-timeout poll parked forever.

## Disposition

Fixed: degrade to always-ready when no path to a COVERING completion
exists -- widened at R2 (F2 sharpening) to also cover a NARROWER live op
under sustained OOM (else spurious-wake-and-repark with no progress). A
safe spurious wake the app re-checks, never a hang.
