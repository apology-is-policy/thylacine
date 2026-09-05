---
id: arc-phase6-pouch
type: arc
title: "Phase 6 — pouch, the POSIX libc"
status: complete
start: 2026-05-20
end: 2026-05-26
chunks:
  - chg-2026-05-22-p6-syscall-seam
  - chg-2026-05-23-p6-mem-b
  - chg-2026-05-23-p6-threads-b
  - chg-2026-05-23-p6-poll
  - chg-2026-05-23-p6-sockets
  - chg-2026-05-24-p6-signals-b
created: 2026-08-01
---
The arc that made Thylacine practical: vendor musl 1.2.5 pristine, then
replace its lower half with Thylacine-native code through an ordered
patch series. Sub-chunks 1-14 -- the vendoring + inventory, the syscall
seam, the compiler runtime, the anonymous-memory backend, pthreads over
torpor, poll/select, AF_UNIX over `/srv`, POSIX signals over kernel
notes, and libsodium cross-compiled into the sysroot.

The proximate goal was running real stratumd; the durable deliverable is
the cross-compilation path itself. Every later pouch landing --
[[arc-pouch-boot]]'s FS syscalls, [[arc-net]]'s socket families,
[[arc-clade]]'s toolchain wires -- extends this series rather than
replacing it.

Surface: [[moc-pouch-seam]].
