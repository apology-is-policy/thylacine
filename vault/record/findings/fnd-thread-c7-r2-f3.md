---
id: fnd-thread-c7-r2-f3
type: fnd
round: adt-thread-c7-r2
severity: P3
status: fixed
title: "5h named a departed thread's read 'not found' where a program sees EIO"
surface: [sub-kernel-devproc]
threatens: []
fixed-by: chg-2026-10-29-thread-model-c7
created: 2026-10-29
---
## Prosecution

The read returns the bare -1 the boundary reports as EIO, as manual 13 says; 5h and the devproc dossier now say so.
