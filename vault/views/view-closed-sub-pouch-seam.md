---
id: view-closed-sub-pouch-seam
type: view
title: "Do-not-re-report preamble — sub-pouch-seam"
query: closed:sub-pouch-seam
---
# Do-not-re-report preamble — sub-pouch-seam

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-seam`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH one standing fact: the recurring defect on this surface is
not in the seam's logic but in its **drift gate**. The build's
seam-check list has been found un-extended in two separate rounds
([[fnd-threads9b-r1-f5]], [[fnd-signals13b-r1-f1]]) — same bug, same
codebase, one round apart, with the first already in the closed list. A
prosecutor finding it a third time has found a process failure, not a new
bug: the obligation belongs to any patch that adds a number.

The other standing fact is [[fnd-seam-r1-f1]]'s shape — a guard on the
path you are reading is not a guard on the mechanism. Two syscall paths
exist.

<!-- generated:begin -->
9 closed findings on [[sub-pouch-seam]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-b1b-r1-f1]] [P2] The found bug was a mis-attribution: __init_tls's raw six-argument SYS_mmap2 never killed a Pouch program (fixed) — Fixed in the close. Every site now says what is true: 0046 is required by
- [[fnd-pouchb0-r1-f1]] [P1] sysconf(_SC_OPEN_MAX / _SC_CHILD_MAX) returns uninitialised stack -- 0034's defect, sixty lines up in the same function, and four more of the same shape (fixed) — Fixed in 0037, each with the honest answer its API allows; `getdtablesize()` alone has no error channel and states the kernel's handle-table size, which the prover MEASURES rather than mirrors. The sweep method in [[sub-pouch-seam]] was rewritten in two halves (half b = unchecked wrapper calls). Round 2 found a sixth member ([[fnd-pouchb0-r2-f1]]).
- [[fnd-pouchb0-r1-f2]] [P2] tmpfile() never unlinks: the raw SYS_unlinkat is a sentinel whose result is ignored, so every tmpfile() leaves /tmp/tmpfile_XXXXXX on the persistent root (fixed) — The suggested fix was tried first and turned the BOOT red: `raw read=-1 errno=2` at EOF of the now genuinely unlinked file. Stratum rejects I/O on a fid whose file was unlinked, by design (`specs/fid.tla` IOReject; `verify_fresh_snapshot`), so an open file does not outlive its last name here; the prover's "the fid survives the unlink" leg had been green since 0024 only because no unlink was ever issued. 0036 is therefore DELETE-ON-CLOSE: the name goes at `fclose()` through `f->close` and, best effort, at a normal `exit()`. Round 2 then found that patch's locking wrong twice and it was restructured (the lock is never held across a syscall). The system-level question -- unlink-while-open loses the file -- is the operator's.
- [[fnd-pouchb0-r1-f4]] [P2] the stdio backends hand a pouch SOCKET fd to the kernel raw: fdopen(sock) never worked and fclose() strands the slot (fixed) — Fixed in 0038: read / write through `pouch_sock_kernel_fd`, close through `pouch_sock_close`, `ESPIPE` for a seek. The census that followed claimed every remaining tag-unaware call fails visibly; round 2 showed that false for `FD_SET` and `ppoll` ([[fnd-pouchb0-r2-f2]]).
- [[fnd-pouchb0-r2-f1]] [P2] ualarm() still returns uninitialised stack -- a sixth member of the class 0037 says it swept, and the closed list states the opposite (fixed) — Fixed in 0037: zero-initialised, and `(unsigned)-1` with the wrapper's ENOSYS when the timer cannot be set (glibc's answer). The closed list's sentence corrected.
- [[fnd-seam-r1-f1]] [P0] The cancellable syscall path had no sentinel guard — a retargeted cancellation point issued svc with x8=0xFFFF (fixed)
- [[fnd-seam-r1-f6]] [P3] `patch -t` silently skips an already-applied patch (fixed)
- [[fnd-signals13b-r1-f1]] [P1] The seam-check list was not extended for the five note syscall numbers (the threads-round F5, verbatim) (fixed)
- [[fnd-threads9b-r1-f5]] [P2] The build's seam-check list was not extended for the round's four new syscall numbers (fixed)
<!-- generated:end -->
