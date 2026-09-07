---
id: chg-2026-09-06-net-clients-httpd-color
type: chg
title: "sub-net-clients de-stale: httpd's access-log --color default flips to auto (H-1c-2, real SYS_FD_DEVCLASS TTY check)"
date: 2026-09-06
arc: arc-vault
commits: ["a596d2d0"]
touched:
  - sub-net-clients
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-net-clients]] (updated 2026-08-04) -- of its eight files, only
`usr/httpd/src/main.rs` changed post-dossier, via H-1c-2 `8922ccd7` (2026-09-01),
the same --color=auto unification whose coreutils side went into
[[chg-2026-09-06-coreutils-filters-destale]] and [[chg-2026-09-06-utopia-eval-shell-arc-notes]].
The dossier never documented httpd's log colour, so nothing was wrong -- this
adds the now-real behaviour.

httpd's access-log `--color` default flipped `Always` -> `Auto`, and the
`stdout_is_console()` stub (hardcoded `true`) became
`libthyla_rs::stdout_is_terminal()` -- the real Dev-class TTY check over
`SYS_FD_DEVCLASS` (Dev class `'c'`), which H-1 landed to retire the long-parked
stub. Net effect: a log piped to a file or another program no longer carries SGR
escapes; colour appears only on an interactive console. Folded one sentence into
the file-server Mechanism paragraph.

`updated:` -> 2026-09-06. Stale backlog 29 -> 28.
