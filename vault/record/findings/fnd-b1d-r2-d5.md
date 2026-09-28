---
id: fnd-b1d-r2-d5
type: fnd
title: "joey handed bin/system.key to the single-component t_walk_open, and both key probes got -EINVAL"
round: adt-b1d-r2
severity: P0
status: fixed
surface: [sub-kernel-joey]
threatens: []
fixed-by: chg-2026-09-25-b1d-round2-close
regression: "joey's boot probes: the 16b-gamma key sanity and #81's T_OPATH leg fail the boot (WIP 8: test.sh 1720/1720)"
created: 2026-09-25
---
## Prosecution

WIP 7 moved `system.key` into the initrd's `bin/` and pointed joey's two key
probes, the 16b-gamma fstat/lseek/wstat sanity and #81's O_PATH read denial,
at `bin/system.key` through `t_walk_open(T_WALK_OPEN_FROM_ROOT, ...)`.
SYS_WALK_OPEN walks one component, and `kernel/syscall.c:2997` refuses a `/`,
so both probes got -EINVAL and the boot failed at the first. The literal census
that drove the move searched for `"/<name>"` and could not see a name handed
to a primitive whose grammar is narrower than a path's. The reviewer read joey
past the point where the first boot had died and reported it while the second
boot was on its way to the same failure.

## Disposition

Fixed in WIP 8 (0119d242): `walk_open_in_bin` walks `bin` as an O_PATH
navigation base, walks the name from it, and closes the base on both outcomes;
#81's leg still proves the O_PATH read denial (verified by [[adt-b1d-r3]]).
