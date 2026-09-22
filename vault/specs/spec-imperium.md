---
id: spec-imperium
type: spec
title: "imperium.tla — bounded elevation flow and teardown publication"
models: [sub-kernel-caps, sub-kernel-proc, sub-imperium]
pins: [inv-i2, inv-i25]
cfgs: [imperium.cfg, imperium_buggy_flow_without_flag.cfg, imperium_buggy_straggler.cfg, imperium_buggy_nest.cfg, imperium_buggy_retag.cfg]
gate: "Run the clean configuration and all four mutant configurations when changing propagation, redemption or teardown publication. Check mutant violations, not merely nonzero exit codes."
created: 2026-09-17
updated: 2026-09-17
---
## Abstraction boundary

Models granted cap sets, scoped propagation, fork publication and root teardown.
It abstracts syscall marshalling, cryptography, console delivery, byte relays,
registry quotas and scheduler progress.

## Action ↔ site map

Grant/redeem maps to devcap and `proc_become_legate`; fork capability flow and
publication map to `rfork_internal`; teardown maps to
`proc_legate_teardown_if_root`. The exact mapping is in `specs/SPEC-TO-CODE.md`.

## Validation

The integration clean run explored 157839 distinct states. All four mutants
were checked for their intended invariant violations. Runtime tests remain
necessary to validate the model's connection to compiled code.
