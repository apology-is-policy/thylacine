---
id: chg-2026-09-28-lr1-la-realm
type: chg
title: "LR-1: a remote mount says so -- the declaration rides the 9P session, /proc/<pid>/ns ends its lines in remote, and ls/stat/realm/ns read it"
date: 2026-09-28
arc: arc-net
commits: ["*(pending)*"]
touched:
  - sub-haul
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-devsrv
  - sub-kernel-srvconn
  - sub-kernel-ninep-attach
  - sub-kernel-ninep-client
  - sub-kernel-ninep-dev9p
  - sub-kernel-territory
  - sub-coreutils-lib
  - sub-coreutils-presenters
  - sub-substrate-interactive
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-28
---
The operator's `la` vote made code. A mount point gets a REALM of its own,
`remote` for a network mount and `mount` for a local one, and the
declaration rides the 9P session in the identity cape's shape:
`SYS_ATTACH_9P_REMOTE` on the pipe attach, `DMSRVREMOTE` on a /srv post in
either mode, inherited by every attach over the service
([[dec-2026-09-28-remote-label-carrier-r2]], which restates and replaces
[[dec-2026-09-28-remote-label-carrier]]: the old note's Fork said `ns`
called the Haul mount's source `disk`, a claim read from code that the
first boot refuted -- every 9P session root is named `/`). The kernel reads
the declaration in one place, `territory_format_ns`, which ends a member's
line in ` remote` and never the covered entry's ([[sub-kernel-territory]]).
Haul declares on both paths ([[sub-haul]]); `ls -l`, `stat` and `realm`
read the caller's own mount list ahead of the fstat inference, and `ns`
defaults to the caller, reads `9p` for `#9`, and gains a FLAGS column
([[sub-coreutils-lib]], [[sub-coreutils-presenters]]). One audit round
(Fable 5.1 reviewing Opus 5.5) closed 0/1/0/2: the `/` label, a cut mount
list now said aloud, and a witness for the priv-magic arm. Fifteen kernel
mutants, each red on its predicted test across six boots; `ergo-1`,
`haul-npxf` and `haul-post` green on the CI image, and red with the
declaration stripped from Haul or the mount list ignored
([[sub-substrate-interactive]]).
