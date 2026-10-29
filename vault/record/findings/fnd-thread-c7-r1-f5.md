---
id: fnd-thread-c7-r1-f5
type: fnd
round: adt-thread-c7-r1
severity: P3
status: deferred
title: "devproc and devramfs emit the qid type as the dirent type byte, which getdents64 forwards as d_type"
surface: [sub-kernel-devproc]
threatens: []
seam: seam-dirent-type-is-qid-type
created: 2026-10-29
---
## Prosecution

Pre-existing: a directory's type byte is QTDIR (0x80) where Linux d_type wants DT_DIR (4), so a pouch or phenotype getdents64 over /proc sees 0x80. Enqueued in OPEN-BUGS (2026-10-29T09:22:54Z) as owned work and recorded as [[seam-dirent-type-is-qid-type]]; not C7's to fold.
