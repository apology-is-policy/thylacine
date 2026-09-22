---
id: fnd-pouchb0-r1-f1
type: fnd
title: "sysconf(_SC_OPEN_MAX / _SC_CHILD_MAX) returns uninitialised stack -- 0034's defect, sixty lines up in the same function, and four more of the same shape"
round: adt-pouchb0-r1
severity: P1
status: fixed
surface: [sub-pouch-seam]
threatens: []
fixed-by: chg-2026-09-21-pouch-b0-libc
regression: "pouch-hello-malloc: sysconf(_SC_OPEN_MAX) and (_SC_CHILD_MAX) == -1 with a pre-loaded errno untouched; getloadavg / ulimit / getdomainname == -1; getdtablesize() == the handle table MEASURED by opening until refusal. RED measured on the unfixed libc: _SC_OPEN_MAX=2116292 errno=38"
created: 2026-09-21
---
## Prosecution

**File**: patched `src/conf/sysconf.c` (the RLIM arm), `src/legacy/{getloadavg,getdtablesize,ulimit}.c`, `src/misc/getdomainname.c`
**Invariant**: Pouch P-3 -- no libc call may fabricate a value
**Prosecution**:
1. `getrlimit`, `sysinfo` and `uname` are wrappers over syscalls the seam parks at the 0xFFFF sentinel; each returns -1 / ENOSYS WITHOUT writing its out-struct.
2. Each caller ignores the return and reads the struct: `sysconf` clamps stack residue to LONG_MAX and clobbers errno; `getloadavg` returns n "samples" of garbage as SUCCESS (GNU make -l N throttles on them); `getdtablesize` and `ulimit` return garbage; `getdomainname` runs strlen / strcpy over an uninitialised `struct utsname`.
3. 0034 fixed exactly this for `_SC_PHYS_PAGES` and stopped there. The author's sweep grepped for statement-position `__syscall(`; these are unchecked WRAPPER calls, which is what 0034's bug was.
**Suggested fix**: check the wrapper; answer -1 where the API has an error channel.

## Disposition

Fixed in 0037, each with the honest answer its API allows; `getdtablesize()` alone has no error channel and states the kernel's handle-table size, which the prover MEASURES rather than mirrors. The sweep method in [[sub-pouch-seam]] was rewritten in two halves (half b = unchecked wrapper calls). Round 2 found a sixth member ([[fnd-pouchb0-r2-f1]]).
