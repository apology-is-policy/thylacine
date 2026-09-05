---
id: chg-2026-05-14-p5-mount-syscall
type: chg
title: "P5-mount-syscall: SYS_MOUNT / SYS_UNMOUNT + the Plan 9 cclose fix"
date: 2026-05-14
arc: arc-phase5-namespace
commits: ["bb778e6b"]
touched:
  - sub-kernel-territory
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
`SYS_MOUNT = 14` / `SYS_UNMOUNT = 15` — thin wrappers over the C API,
with the `_for_proc` inner / static SVC wrapper split that makes the
handler kernel-testable. The rights gate is `RIGHT_READ` on the source
handle and nothing else; the target is still an abstract `path_id_t`
(stalk-2 re-keys it).

The chunk's real content is the bundled **`spoor_clunk` fix**: from "run
`dev->close` on every clunk" to Plan 9 `cclose` semantics — run it ONLY
on the last drop. The prior behaviour tore down per-Spoor Dev state
(pipe endpoints, 9P sessions) on the user's FIRST close even while a
mount-table entry still held a reference, which directly contradicts the
lifecycle the mount table promises ("close the attach fd after mounting;
the table holds the ref").

The lesson is why it was bundled rather than split. The eager-close
behaviour had been indistinguishable from correct through every prior
chunk, because every prior chunk had `ref == 1` holders — there was no
shape in which the difference was observable. The same feature that
introduces the multi-holder pattern is the one that must fix it, or it
lands broken; splitting would have left a commit whose tests pass for a
reason the next commit invalidates. Three `spoor_unref` -> `spoor_clunk`
conversions in `territory.c` (MREPL displacement, `unmount`, final
release) accompany it — each is a "releasing my holder" site, which is
exactly what `cclose` is for.
