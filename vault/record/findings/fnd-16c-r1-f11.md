---
id: fnd-16c-r1-f11
type: fnd
title: "init_destroy test's manual unref double-drop hazard"
round: adt-16c-r1
severity: P3
status: documented
surface: [sub-kernel-ninep-transport]
threatens: []
created: 2026-07-31
---
## Prosecution

If a future maintainer adds an unref to `_destroy()`, the test's manual
`srvconn_unref` becomes a double-drop.

## Disposition

Documented (the close-before-destroy discipline comment carries it);
refcount asserts judged non-load-bearing. Test hygiene, not system debt --
no seam.
