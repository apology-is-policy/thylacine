---
id: chg-2026-09-29-ns-session-root-names
type: chg
title: "/proc/<pid>/ns names a 9P session root by the file its session came over -- a display-only origin on the root's dev9p priv"
date: 2026-09-29
arc: arc-net
commits: ["3936063b"]
touched:
  - sub-kernel-ninep-dev9p
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-territory
  - sub-coreutils-lib
  - sub-coreutils-presenters
  - sub-haul
  - sub-substrate-interactive
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-29
---
Operator vote 1 of 2026-09-28 made code
([[dec-2026-09-28-ns-session-root-names]]). Every 9P session root read `/` on
its mount line -- login's home was `mount /home/michael /` -- because that is
the name a root is born with, and the root has to keep it: joey pivots to a
`t_attach_9p_srv` root, and a pivot never re-stamps a published Spoor (I-33).
So the root carries a second, display-only name, `origin` on its dev9p priv: a
ref on the transport file's name, or `origin_dc`, that file's device char when
it has none ([[sub-kernel-ninep-dev9p]]). Both attach handlers stamp it before
`handle_alloc` publishes the root, from the connection for `SYS_ATTACH_9P_SRV`
and from the transmit pipe for `SYS_ATTACH_9P`, no argument changed
([[sub-kernel-syscall-dispatch]], [[sub-kernel-syscall-abi]]);
`territory_format_ns` is its one reader, and never renders it on a covered
entry ([[sub-kernel-territory]]). A walk builds a fresh priv, so no walked or
cloned Spoor carries one. Login's home now reads `mount /home/michael
/srv/home-michael`, the shell's mount of a posted Haul service `/srv/NAME`,
and Haul's private form, whose session rides pipes, `#|`; `ns` gives `#|` the
REALM `9p` ([[sub-coreutils-lib]], [[sub-coreutils-presenters]],
[[sub-haul]]), and both Haul gates assert the new line
([[sub-substrate-interactive]]). One audit round (Fable 5.1 reviewing Opus
5.5) was clean. Nine kernel mutants, each red on its predicted test across six
boots, and one coreutils mutant red on its host test; with both stamps
removed, `haul-npxf` and `haul-post` failed at their `ns` leg, the line
reading `/`. `tools/test.sh` 1753/1753, zero `[skip]`; `ci-smp-gate`
(default-smp4, ubsan-smp4, N=10) 10 + 10 boots PASS, no corruption, external
kill, timing or other.
