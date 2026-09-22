---
id: chg-2026-09-21-pouch-b0-libc
type: chg
title: "Pouch 0033-0041: nine libc patches a JavaScript engine shook out -- lies, not errors"
date: 2026-09-21
arc: arc-boosty
commits: ["92a9ae94"]
touched: [sub-pouch-seam, sub-pouch-net, sub-pouch-fs, sub-pouch-thread, sub-kernel-exec]
established: []
closed: [fnd-pouchb0-r1-f1, fnd-pouchb0-r1-f2, fnd-pouchb0-r1-f3, fnd-pouchb0-r1-f4, fnd-pouchb0-r2-f1, fnd-pouchb0-r2-f2]
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-21
---
**What.** Nine patches, eight of them one defect class: a syscall the seam parks at the
`ENOSYS` sentinel whose libc caller cannot or does not report the failure,
so the program is told a VALUE instead of an error. 0033 the main thread's
stack bounds (one page reported; JSC's recursion limit), 0034
`sysconf(_SC_PHYS_PAGES)` (uninitialised stack; JSC's heap cap), 0035 the
stdio read backend that never refilled its buffer (every `fscanf` on a real
`FILE` failed at the first pushed-back delimiter), 0036 `tmpfile()`
delete-on-close, 0037 six unchecked wrappers, 0038 stdio over a socket fd,
0039 the `fd_set` guard + a tag-aware `ppoll`, 0040 `O_APPEND` as the
kernel's omode bit. The ninth, 0041, is the libc half of a kernel fix
([[chg-2026-09-21-srvconn-two-endpoint-poll]]): `poll()` gives a connected
AF_UNIX socket the stream-socket shape at EOF.

**Why it is one chunk.** Each patch was found by the pin of the one before
it or by the audit of it: 0034's device pin found 0035; the audit of 0035
found 0036-0038; the audit of those found 0039-0040 and the sixth member of
0037's class. Three prosecutor rounds ([[adt-pouchb0-r1]],
[[adt-pouchb0-r2]], [[adt-pouchb0-r3]]) returned no P0
and one P1 per round at most, and every round's findings were a surviving SIBLING of a fixed
defect plus FALSE SENTENCES written the same day as the fix.

**Alternatives rejected.** Create-then-unlink for `tmpfile()` (the audit's
own suggestion) turned the boot red: Stratum rejects I/O on a fid whose
file was unlinked, by design (`specs/fid.tla` IOReject), so an open file
does not outlive its last name on this root. That is a filesystem-semantics
question and is the operator's. Small-integer socket fds (the real fix
behind 0039) are a redesign of 0006 / 0016 with their own audit, owed.

**Verification.** The series (41 patches) applies to pristine musl with
`tools/build.sh`'s exact flags and no fuzz, offset or reject -- measured
under `--fuzz=0` after 0024 turned out to have no trailing newline, which
BSD `patch` absorbs silently and `tools/check-patch-hunks.py` now fails; 0035
re-verified against the REAL patched sources, 0 failures in 288,000
trials, with both positive controls discriminating; every new prover leg
green on the device and measured RED on the unfixed libc where a device
run can reach it; joey matches each prover on a leg census so a stale
binary cannot pass. Closed list: `memory/audit_pouch_0033_0035_closed_list.md`.
