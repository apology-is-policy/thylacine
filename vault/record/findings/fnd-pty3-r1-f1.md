---
id: fnd-pty3-r1-f1
type: fnd
round: adt-pty3-r1
severity: P3
status: fixed
title: "put_dec's scratch was sized for its call sites, not for its signature"
surface: [sub-pouch-tty]
threatens: []
fixed-by: chg-2026-07-18-pty3
created: 2026-08-01
---
## Prosecution

`put_dec(char *cmd, int len, unsigned v)` used a `char dec[8]` scratch,
which cannot hold a 10-digit unsigned. Unreachable at v1.0 — both callers
pass `unsigned short` (<= 5 digits) — but the helper's SIGNATURE accepts
any unsigned, so its bound is a property of its callers rather than of
itself.

The self-audit reasoned from the call-site count and stopped; the formal
round read the signature.

## Fix

`dec[10]`.
