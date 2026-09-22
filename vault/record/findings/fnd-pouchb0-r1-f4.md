---
id: fnd-pouchb0-r1-f4
type: fnd
title: "the stdio backends hand a pouch SOCKET fd to the kernel raw: fdopen(sock) never worked and fclose() strands the slot"
round: adt-pouchb0-r1
severity: P2
status: fixed
surface: [sub-pouch-net, sub-pouch-seam]
threatens: []
fixed-by: chg-2026-09-21-pouch-b0-libc
regression: "pouch-hello-sockets: fdopen() the connected client end, fputs / ppoll / fscanf (pushback) / fgets / fseek == ESPIPE / fclose, then the free-slot count measured before == after. The on-device RED was not measurable (the prover needs joey's post-service permission); discrimination rests on the host model and the saved old-libc binary"
created: 2026-09-21
---
## Prosecution

**File**: patched `src/stdio/__stdio_{read,write,close,seek}.c`
**Invariant**: every fd-consuming call must be tag-aware ([[sub-pouch-net]])
**Prosecution**:
1. A pouch socket fd is `0x40000000 | slot`, a libc-side value the kernel never issued.
2. The four `FILE` backends issue raw syscalls on `f->fd`, so they are fd consumers a sweep of the public wrappers never sees.
3. `fdopen(sock)` yields a stream on which every operation fails (EBADF -> F_ERR); `fclose()` "closes" a number the kernel does not know and STRANDS the slot with its kernel handles. The table is `POUCH_SOCK_MAX` = 8 wide: eight such closes end `socket()` for the life of the Proc.
4. Pre-existing; preserved by 0035's rewrite of `__stdio_read`.
**Suggested fix**: map the tag as read / write / close already do.

## Disposition

Fixed in 0038: read / write through `pouch_sock_kernel_fd`, close through `pouch_sock_close`, `ESPIPE` for a seek. The census that followed claimed every remaining tag-unaware call fails visibly; round 2 showed that false for `FD_SET` and `ppoll` ([[fnd-pouchb0-r2-f2]]).
