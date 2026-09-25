---
id: fnd-b1d-r2-f1
type: fnd
title: "The dlopen prover's ../ escape leg stopped testing an escape when the working directory moved to /bin"
round: adt-b1d-r2
severity: P2
status: fixed
surface: [sub-pouch-mem]
threatens: []
fixed-by: chg-2026-09-25-b1d-round2-close
regression: "pouch-hello-dlopen confined leg: chdir(\"/\") + the pre-pivot open of the same relative name; the post-pivot open(\"..\") control (fnd-b1d-r3-f1); sabotage U4 (no pivot) RED"
created: 2026-09-25
---
## Prosecution

The confined leg pivots a child to `/srv` and requires the bare name, the
absolute path and `../lib/libdlprobe.so` to miss with ENOENT. While the
initrd was flat the child's working directory was `/`, so the relative name
joined to `/../lib/X` and the walk reached the resolver's floor. After the bin/
move the child inherited joey's `/bin`. The dot is a name (LS-4; `cwd_join`
copies it verbatim, #83), so the name joined to `/bin/../lib/X`, and in the
pivoted root the walk missed at `bin` before the `..` was reached. ENOENT
either way: the leg passed without testing an escape.

## Disposition

Fixed. The child chdirs to `/` first and, before the pivot, opens the same
relative name, a control one variable away on the pivot axis. Round 3 found
that no control saw the chdir itself removed ([[fnd-b1d-r3-f1]]).
