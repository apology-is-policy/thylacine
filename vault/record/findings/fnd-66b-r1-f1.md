---
id: fnd-66b-r1-f1
type: fnd
title: "The single-hop walk-open adoption arm has no dedicated regression"
round: adt-66b-r1
severity: P3
status: deferred
surface: [sub-kernel-path]
threatens: []
seam: seam-66c-proc-fd
regression: "partial — stalk.path_adopt_transplant covers the multi-hop arm; the syscall arm is uncovered"
created: 2026-08-01
---
## Prosecution

The #66a-owed transplant regression landed as
`stalk.path_adopt_transplant`, which covers the MULTI-HOP arm in
`stalk.c`. The identical one-liner in `sys_walk_open_handler` — the
single-hop arm that `File::open("/srv/<name>")` actually hits when a Dev
whose open REPLACES the quarry (devsrv's open=connect) returns a
nameless replacement — has no test of its own.

The MECHANISM is proven: the same `spoor_path_transplant` call, on the
same shape, verified non-vacuously at the other arm. What is untested is
that this particular call site is present and correctly placed.

## Disposition

DEFERRED with explicit justification, not dropped. A dedicated test
needs net-new fd/handle-table harness infrastructure — no existing
kernel test drives `sys_walk_open_for_proc` together with
`sys_fd2path`, grep-confirmed — which is disproportionate for a P3 gap
on a fail-soft ([[inv-i33]]) surface where the worst outcome is a blank
name in an introspection file.

Tracked to [[seam-66c-proc-fd]], where connection-fd names are exercised
end to end and the harness has to exist anyway.
