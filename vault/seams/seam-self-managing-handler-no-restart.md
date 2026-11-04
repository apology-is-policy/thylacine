---
id: seam-self-managing-handler-no-restart
type: seam
title: "A self-managing Proc with a registered handler does not get its row's restart"
status: open
surface: [sub-kernel-notes]
opened-by: fnd-thread-11c-r1-f4
tracker: "OPEN-BUGS, item 11c"
created: 2026-11-03
updated: 2026-11-03
---
## Owed

The tail's table branch is skipped for a self-managing Proc, so `row_flags` stays 0 and
`notes_call_settle` refuses the restart. The program sees EINTR where its row asked for a
restart. POSIX allows EINTR there, and no in-tree program both opens its notes fd and
installs a handler.

## What closes it

The first program that does both, or a tail that reads the row before it asks whether the
Proc manages its own notes.
