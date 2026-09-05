---
id: fnd-99-r1-f5
type: fnd
title: "The Go-side retry is one-shot under a create/unlink storm"
round: adt-99-r1
severity: P3
status: documented
surface: [sub-kernel-ninep-dev9p]
threatens: []
created: 2026-07-31
---
## Prosecution

A create/unlink storm racing the open-or-create path could surface ENOENT
where POSIX loops. (The defect home is the go fork's file_thylacine.go
retry -- recorded here because the round's scope was this surface and no
go-fork vault node exists.)

## Disposition

Documented: vanishing once F1 is fixed kernel-side (10/10); the bounded
Open/Create loop is the fuller shape only for a workload v1.0 does not
exercise -- noted in file_thylacine.go.
