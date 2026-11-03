---
id: fnd-thread-11c-r1-f4
type: fnd
round: adt-thread-11c-r1
severity: P3
status: deferred
title: "A self-managing Proc with a registered handler does not get its row's restart"
surface: [sub-kernel-notes]
threatens: []
seam: seam-self-managing-handler-no-restart
created: 2026-11-03
---
## Prosecution

The tail's table branch is skipped for a self-managing Proc, so the row's flags stay 0 and settling refuses the restart. It sees EINTR, which POSIX allows, where its row asked for a restart. No in-tree program does both: Pouch names `note_open` only in its number map, and libthyla-rs natives have no handler.
