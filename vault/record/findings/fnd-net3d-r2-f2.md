---
id: fnd-net3d-r2-f2
type: fnd
title: "The gen-guard comment overstated necessity — it is the belt against a FUTURE refcount-pin regression"
round: adt-net3d-r2
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

With the F1 fixes in place the listen fid pins N's refcount for the
pending's whole lifetime, so the gen arm of the poll_accepts guard is
strictly redundant TODAY; the comment claimed it load-bearing.

## Disposition

The comment now states the truth: the proto arm makes the typed get
locally sound regardless, and the gen arm is the belt against a future
refcount-pin regression — precision that prevents a later "remove the
redundant guard" cleanup from deleting the belt.
