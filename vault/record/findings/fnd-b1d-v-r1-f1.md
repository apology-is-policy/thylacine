---
id: fnd-b1d-v-r1-f1
type: fnd
title: "alloc-smoke's directory success path mounted /lib, which only an LLVM-fork image ships"
round: adt-b1d-v-r1
severity: P2
status: fixed
surface: [sub-kernel-territory]
threatens: []
fixed-by: chg-2026-09-25-b1d-v-emount
regression: "usr/alloc-smoke U-2f (the /bin directory source)"
created: 2026-09-25
---
## Prosecution

WIP 1 turned U-2f's file-over-`/srv` leg into refusal checks and kept a
success path with a directory source, `/lib`. The initrd carries `lib/` only
when the LLVM fork built `libc.so`; `bin/` is required on every image
(`bin/joey`). A probes-ON image built without the fork would fail
`File::open("/lib")`, and alloc-smoke's failure extincts the boot.

## Disposition

Fixed: the directory source is `/bin`. The same leg now mounts it through the
three `bind_*` wrappers and reads each placement back from `/proc/<pid>/ns`.
