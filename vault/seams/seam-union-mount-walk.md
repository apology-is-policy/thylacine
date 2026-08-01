---
id: seam-union-mount-walk
type: seam
title: "MBEFORE / MAFTER / MCREATE are stored and never walked"
status: open
surface: [sub-kernel-territory, sub-kernel-stalk]
opened-by: chg-2026-05-13-p5-attach-mount
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Plan 9's union mount — several sources stacked at one point, walked in
`MBEFORE`/`MAFTER` order, with `MCREATE` marking which member takes new
files. Thylacine stores all four flags in `PgrpMount.flags` and honors
exactly one: `MREPL` replaces the first entry at a matching identity.
The other three are recorded and treated as "append a new entry", and
`mount_lookup` returns the FIRST match regardless of order — so a union
is installable but resolves as if only its first member existed.

## What closes it

Three coupled pieces: an ordering invariant on `mounts[]` (which retires
the swap-remove in `unmount` in favour of shift-down, since order stops
being cosmetic), a `mount_lookup` that yields members in order rather
than the first hit, and a `stalk` cross that tries members in turn on a
walk miss. `MCREATE` additionally needs the create path to pick the
marked member.

The spec's set-valued `mounts` is beneath the ordering, so it would need
a sequence — a real model change, not an added action.

## Risk while open

None today: nothing constructs a union (the boot chain uses MREPL
throughout), so the unordered first-match is always the only match. The
hazard is a caller that BELIEVES it built a union — the flags are
accepted silently, so `mount(..., MAFTER)` succeeds and then behaves as
neither a union nor an error. A fail-closed reject of the unimplemented
flags would be the honest holding action.
