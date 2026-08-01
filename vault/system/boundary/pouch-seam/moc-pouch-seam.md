---
id: moc-pouch-seam
type: moc
title: "pouch — the POSIX boundary line"
parent: moc-boundary
created: 2026-08-01
updated: 2026-08-01
---
pouch is Thylacine's C library: musl 1.2.5 vendored pristine at
`third_party/musl/`, with a **patch series** (`usr/lib/pouch/patches/`,
31 patches / ~11.5 kLOC) replacing musl's lower half with Thylacine-native
code. The upper half — printf, qsort, strtol, the math library, mallocng's
allocation logic — is musl's and stays musl's. The boundary line is musl's
own syscall seam, which is what makes a patch series (rather than a fork)
tractable: upstream's security fixes remain a re-vendor-and-rebase away.

pouch is not a Linux emulation layer. Below its own boundary it carries no
Linux syscall numbers and no Linux kernel-ABI assumptions.

## Children

- [[sub-pouch-seam]] — the seam itself: the number table, the
  unimplemented-syscall sentinel, the error decode, the stdio backend.
- [[sub-pouch-fs]] — open / stat / readdir / the mutation family /
  readlink: POSIX paths over the stalk resolver + the parent-fd+leaf
  primitives.
- [[sub-pouch-thread]] — pthreads over `SYS_THREAD_SPAWN` + torpor, and
  sleeping over the same wait-on-address primitive.
- [[sub-pouch-process]] — `posix_spawn` / wait / pipe / dup, the `/env`
  environ populate, and the two termination overrides.
- [[sub-pouch-signal]] — POSIX signals over kernel notes.
- [[sub-pouch-net]] — poll/select, AF_UNIX over `/srv`, AF_INET over
  `/net`.
- [[sub-pouch-tty]] — the tty ioctl dispatcher: pts and console.

## The four pouch invariants

Binding, from POUCH-DESIGN.md §11 (cross-referenced from ARCHITECTURE.md
§28, but NOT numbered there — they constrain one subsystem, not the
system):

- **P-1** — no foreign syscall number ever reaches the kernel.
- **P-2** — pouch is the sole POSIX path; the kernel makes zero POSIX
  accommodations.
- **P-3** — no silently-wrong POSIX: every surface either maps to a
  defined Thylacine behavior or returns a documented `errno`.
- **P-4** — the boundary line holds: the upper half carries no
  Thylacine-specific code; the patch series touches only the lower half
  and the seam.

P-1 and P-3 are structurally enforced by one mechanism ([[sub-pouch-seam]]'s
sentinel); P-4 is enforced by review against the UPPER/LOWER/SEAM
inventory. P-2 is a kernel-side property — its evidence is the absence of
socket, signal, and terminal syscalls in `kernel/syscall.c`.

## Cross-cutting

- **The sentinel is the ledger.** An un-retargeted POSIX call compiles,
  links, and returns `ENOSYS` at runtime. "It builds" therefore never
  means "it works"; the `0xFFFF` entries in `bits/syscall.h.in` are the
  live inventory of what pouch has not done yet.
- **Every patch stacks.** The series is quilt-ordered (`series`), and
  later patches routinely rewrite earlier patches' files — `openat.c` has
  three generations (0009 → 0021 → 0024), `_pouch_socket.h` five. Reading
  one patch tells you what changed, never what the file now says.
- Kernel surfaces pouch stands on: [[moc-kernel-namespace]] (stalk
  resolution), [[moc-kernel-ipc-wake]] (torpor, poll), [[moc-kernel-srv]]
  (`/srv`), [[moc-userspace-netd]] (`/net`).
