---
id: fnd-stalk1-r1-f1
type: fnd
title: "amode was unvalidated — an unknown mode silently degraded to walk-only"
round: adt-stalk1-r1
severity: P3
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-06-02-stalk1
created: 2026-08-01
---
## Prosecution

`stalk()` dispatched on `amode` only at the final hop; any value outside
{STALK_WALK, STALK_OPEN} behaved as walk-only. Latent for every future
amode (stalk-2's STALK_MOUNT, POUNCE's STALK_STAT): a caller passing a
new mode into an old kernel — or a missed dispatch arm — would silently
skip an open, a cross, or a create's parent check instead of failing.

## Disposition

Fixed in the close: the entry guard rejects any unknown amode LOUDLY
(NULL). The standing obligation it created — a new amode MUST extend the
guard AND land its final-hop arm, fail-closed — is carried in the
[[sub-kernel-stalk]] Prosecution list and was honored by both later
amode additions.
