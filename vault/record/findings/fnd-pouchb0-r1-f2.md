---
id: fnd-pouchb0-r1-f2
type: fnd
title: "tmpfile() never unlinks: the raw SYS_unlinkat is a sentinel whose result is ignored, so every tmpfile() leaves /tmp/tmpfile_XXXXXX on the persistent root"
round: adt-pouchb0-r1
severity: P2
status: fixed
surface: [sub-pouch-seam]
threatens: []
fixed-by: chg-2026-09-21-pouch-b0-libc
regression: "pouch-hello-fopen tmpfile leg: the /tmp/tmpfile_* count before, +1 while open (the positive control), unchanged after fclose, unchanged after a self-respawned child exits 42 with a tmpfile open; three pages round-tripped to a clean EOF over the wire. RED measured on the unfixed libc with a probe run twice: during=1 nlink=1 after_close=1, then after_close=2"
created: 2026-09-21
---
## Prosecution

**File**: upstream `src/stdio/tmpfile.c`
**Invariant**: Pouch P-3; the persistent root must not accumulate a file per call
**Prosecution**:
1. Upstream creates the file and at once issues `__syscall(SYS_unlinkat, ...)`, ignoring the result.
2. That number is the 0xFFFF sentinel: no unlink happens.
3. Every `tmpfile()` leaves a named file for ever; the new scan leg doubled the rate. 0027 fixed `remove()` for the same line and missed this twin.
**Suggested fix**: call the public `unlink()`.

## Disposition

The suggested fix was tried first and turned the BOOT red: `raw read=-1 errno=2` at EOF of the now genuinely unlinked file. Stratum rejects I/O on a fid whose file was unlinked, by design (`specs/fid.tla` IOReject; `verify_fresh_snapshot`), so an open file does not outlive its last name here; the prover's "the fid survives the unlink" leg had been green since 0024 only because no unlink was ever issued. 0036 is therefore DELETE-ON-CLOSE: the name goes at `fclose()` through `f->close` and, best effort, at a normal `exit()`. Round 2 then found that patch's locking wrong twice and it was restructured (the lock is never held across a syscall). The system-level question -- unlink-while-open loses the file -- is the operator's.
