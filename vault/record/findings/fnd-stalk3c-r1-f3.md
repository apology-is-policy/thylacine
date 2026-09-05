---
id: fnd-stalk3c-r1-f3
type: fnd
title: "The boot-registry getter comment overstated production reachability — I-1 prosecuted directly and HELD"
round: adt-stalk3c-r1
severity: P3
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-06-03-stalk3c-retire
created: 2026-07-31
---
## Prosecution

The `g_boot_srv_registry` getter's comment claimed broader production
use than remained true post-retirement. The round used it as the entry
point to prosecute per-territory isolation DIRECTLY: could any EL0 path
still bind the boot registry other than through a mounted `/srv` root?

## Disposition

The prosecution came back HELD and STRENGTHENED: post/connect resolve
the registry from the walked root Spoor's aux (both re-validate
`SRV_REGISTRY_MAGIC`), and the retirement REMOVED the only EL0-reachable
functions that bound the global directly — a future per-session registry
is structurally unnameable from outside its territory. Remaining global
users are boot-mount, the exit-notify hook, and tests. Comment corrected
in the F2 sweep. This disposition is the audit half of [[inv-i1]]'s
devsrv edge.
