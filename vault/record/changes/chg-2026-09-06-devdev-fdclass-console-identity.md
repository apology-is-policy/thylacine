---
id: chg-2026-09-06-devdev-fdclass-console-identity
type: chg
title: "sub-kernel-devdev de-stale: devdev_fd_devclass (H-1a fd-class; /dev/cons -> 'c') and spoor_is_console (viv-C2 unforgeable console identity, not a qid bit)"
date: 2026-09-06
arc: arc-vault
commits: ["0c859961"]
touched:
  - sub-kernel-devdev
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-devdev]] (updated 2026-08-16) -- the /dev front-door, `audit: hard`
by the trigger index (I-27) -- missed two settled 2026-09-01 changes to
`devdev.c`, both verified in source. Mixed-track, both settled (main's H-arc
closed 2026-09-06; aux on Nocturne), and both NEW mechanisms the dossier did not
scaffold, so added rather than corrected.

- **`devdev_fd_devclass`** (H-1a `7cd1ab94`, main). The class a devdev-backed fd
  reports to `SYS_FD_DEVCLASS`: only the console DATA leaf (`DEV_KIND_CONS`)
  normalizes to `'c'`, so a `/dev/cons` fd is indistinguishable from a
  `SYS_CONSOLE_OPEN` fd to the is-a-terminal predicate; every control leaf and
  every directory answers devdev's own `'d'`. Rode with it: the consctl mode-line
  read's staging buffer grew `tmp[64]` -> `tmp[96]` because the render gained a
  trailing `beacon <tier>` field and 64 sat below the reserve floor -> every
  consctl read EOF'd (H-1a suite catch).
- **`spoor_is_console`** (viv-C2 `1890283c`, aux; the forgeable-qid-bit fix). Is
  this fd the kernel console, by UNFORGEABLE device identity? Keys on the kernel
  `Dev` pointer -- `sp->dev == &devcons` OR `devdev` + `DEV_KIND_CONS` -- NOT a
  qid bit, because a dev9p Spoor's qid path is server-supplied and tapestryd's
  pane flag occupies the same bit (41) a console qid uses, so a bit-only test
  would accept a forged pane fd (the "a server-settable qid bit is not an
  identity" hazard).

Folded into a new Mechanism subsection ("Two identity questions...") + a
Prosecution bullet (key on the device pointer, never a qid bit). `updated:` ->
2026-09-06. Stale backlog 28 -> 27.
