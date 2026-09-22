---
id: fnd-pouchb0-r2-f2
type: fnd
title: "a pouch socket fd cannot live in an fd_set: FD_SET(sock, &s) is a store 128 MiB past the set, and ppoll() hands the tag to the kernel raw -- while the census says every remaining call fails visibly"
round: adt-pouchb0-r2
severity: P2
status: fixed
surface: [sub-pouch-net]
threatens: []
fixed-by: chg-2026-09-21-pouch-b0-libc
regression: "pouch-hello-sockets: ppoll() on the connected socket with the reply on its way must report POLLIN, not POLLNVAL; a self-respawned child that FD_SETs a socket fd must die with abort's status (127), not return and not fault"
created: 2026-09-21
---
## Prosecution

**File**: patched `include/sys/select.h:27`; `src/internal/_pouch_socket.h`; `src/select/select.c`; `src/select/ppoll.c`; `sub-pouch-net.md`
**Invariant**: memory safety; "every fd-consuming call must be tag-aware"
**Prosecution**:
1. `#define POUCH_SOCK_TAG 0x40000000`; `#define FD_SET(d, s) ((s)->fds_bits[(d)/(8*sizeof(long))] |= ...)` over a 16-long array.
2. `s = socket(...); FD_SET(s, &rfds);` -> index 16,777,216 -> a store 128 MiB past the set, in APPLICATION code, before libc is entered. From the main stack that is usually unmapped; from a heap-resident set it is whatever Burrow sits there.
3. Only afterwards does `select()` refuse (`n > FD_SETSIZE`).
4. `ppoll()` is `syscall_cp(SYS_poll, fds, n, timeout_ms)`: a tagged fd answers POLLNVAL, counts as ready, defeats the timeout, busy-spins.
5. The paragraph added in round 1 says every remaining call "FAILS VISIBLY". The census method ("every site that passes an fd to a raw syscall") cannot see either: select takes a bitmap, ppoll an array.
**Suggested fix**: now, correct the sentence and guard the macros; the real fix is socket fds that are small integers.

## Disposition

Fixed as the honest minimum in 0039: `FD_SET` / `FD_CLR` / `FD_ISSET` `abort()` with a message outside `[0, FD_SETSIZE)` (glibc's `__fdelt_chk`), in a comma form that keeps upstream's value and raises no unused-value warning; `ppoll()` goes through the tag-aware `poll()`. The patch header says it is not the fix. Small-integer socket fds -- a placeholder kernel handle per slot and an fd -> slot side table, retiring the tag from every fd-consuming call -- are a redesign of 0006 / 0016 with their own audit, OWED and recorded in [[sub-pouch-net]].
