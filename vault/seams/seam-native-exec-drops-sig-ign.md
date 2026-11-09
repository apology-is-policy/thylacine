---
id: seam-native-exec-drops-sig-ign
type: seam
title: "A native exec drops an ignored row, where POSIX keeps it"
status: open
surface: [sub-kernel-notes, sub-pouch-signal]
opened-by: chg-2026-11-03-item-11c
tracker: "ARCH 8.8.3, item 11c"
created: 2026-11-03
updated: 2026-11-03
---
## Owed

POSIX keeps `SIG_IGN` across exec, and a Linux guest's table does. A native exec clears
the table and its flag, the Plan 9 rule of ARCH 7.6, so a Pouch program that ignores
`SIGHUP` and then execs, the shape of `nohup`, gives its image the default back. The
same goes for `SIGPIPE` ignored by a shell before it runs a pipeline.

## What closes it

A decision on what crosses a native exec. Keeping the ignored rows and dropping the
live ones, as `viv_sigtab_reset` does for a guest, would keep the table governing past
an exec into a program that never asked for one, and that program's `SYS_NOTIFY` would
then not catch every note. So the row has to be kept apart from the flag, or the flag
set again only by a row the new image wrote.
