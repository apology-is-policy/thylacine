---
id: sub-pouch-seam
type: sub
parent: moc-pouch-seam
title: "The syscall seam — the number table, the sentinel, the error decode, stdio"
code:
  - usr/lib/pouch/patches/0001-pouch-syscall-seam.patch
  - usr/lib/pouch/patches/0002-pouch-stdio-no-iovec.patch
  - usr/lib/pouch/patches/0008-pouch-hw-syscalls.patch
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
design: ["docs/POUCH-DESIGN.md"]
created: 2026-08-01
updated: 2026-08-15
---
## Purpose

The one place a POSIX call becomes a Thylacine syscall. musl funnels
every OS operation through `__syscallN` / `__syscall_cp` with a number
from `arch/aarch64/bits/syscall.h.in`; pouch rewrites that table to
Thylacine numbers, guards the entries it has no answer for, and decodes
the kernel's flat-`-1` error convention into POSIX `errno`. Everything
else in the series is a lower-half file riding this seam.

## Contract

- **`bits/syscall.h.in`** — every `__NR_*` macro carries either a
  Thylacine syscall number or `0xFFFF`, the unimplemented sentinel. The
  file is reproducible after a musl re-vendor by the awk filter recorded
  in the 0001 header (a name→number map; everything unmapped becomes the
  sentinel).
- **`__syscall0..6`** (`arch/aarch64/syscall_arch.h`) — each begins
  `if (n == POUCH_SYSCALL_UNIMPL) return -ENOSYS;` BEFORE issuing `svc`.
- **`__syscall_cp`** (`src/thread/__syscall_cp.c`) — the same guard, at
  the C chokepoint every *cancellable* call resolves through.
- **`__syscall_ret`** (`src/internal/syscall_ret.c`) — `r == -1` →
  `errno = EIO`, return -1; `r` in `[-4095,-2]` → `errno = -r`; else pass
  through.
- **stdio** — `__stdio_write` / `__stdio_read` move bytes with
  `SYS_write` / `SYS_read` instead of `writev` / `readv`.

## Mechanism

**The sentinel is the whole design.** A retargeted-to-`0xFFFF` call is
short-circuited in userspace: no trap is issued, so **P-1** holds
structurally (no foreign number can reach the kernel even by accident),
and the caller sees a clean `ENOSYS` rather than a Thylacine `-1`
meaning something else, so **P-3** holds too. One mechanism, two
invariants, zero runtime cost on the live path.

**Both syscall paths must be guarded.** The guards in `__syscallN` cover
only the non-cancellable path; musl's cancellable path runs a
hand-written `__syscall_cp_asm` that carries no guard, so a retargeted
cancellation-point call (`nanosleep`, `read`, `open`) on a
cancellation-enabled thread would have issued `svc` with `x8 = 0xFFFF`.
That was the seam round's **P0** ([[fnd-seam-r1-f1]]) and the fix is the
C chokepoint guard above.

**Two ways to name a Thylacine number**, and which one a patch uses is
decided by whether Linux has the name at all:

- *Repoint* the macro in `bits/syscall.h.in` — when a Linux name exists
  and means the same thing (`__NR_read 9`), or exists and is being taken
  over (0010's `#undef __NR_fstat` / `#define __NR_fstat 50`, spelled
  loudly so a future reader sees the redefinition). Thylacine-only names
  (torpor, thread_spawn, the note family, the hw family, the tty family)
  are appended here too, so musl's build-time sed pass generates the
  `SYS_*` aliases the patched sources reference.
- *Define locally* as `SYS_thyla_*` in a pouch-private header (0019
  introduced the idiom for `SYS_stat`; 0024 and 0026 adopted it). The
  reason is mechanical: aarch64's LP64 table has no legacy `__NR_stat`
  to repoint, so there would be no generated alias — and churning
  `syscall.h.in` for every new call makes the re-vendor diff worse.

**The error decode's ordering is load-bearing**: `-1` also satisfies
`r > -4096UL`, so the flat-error test must precede the range test or
every Thylacine failure would report `errno = 1` (`EPERM`).

**The `_Static_assert` pins the sentinel's VALUE across two files**
(the `0xFFFF` literal in the table, the `POUCH_SYSCALL_UNIMPL` macro in
`syscall_arch.h`) — and explicitly does NOT witness that the guards
exist; that is the two source patches' job. Its witness is
`SYS_io_setup`: Linux async I/O, which POUCH-DESIGN §8.2 defers forever,
so it can never gain a number and quietly falsify the assert.

**stdio without iovec.** `__stdio_write` loops `SYS_write` over the two
spans musl would have passed to `writev` (the stream's pending buffer,
then the caller's new data), returning the count of *new* bytes on
error — musl's contract. A `cnt <= 0` return is treated as terminal,
which is a deliberate coupling to Thylacine's write semantics (a write
blocks until it makes progress and returns -1 on a dead peer; it never
0-returns for flow control) and avoids the unbounded spin musl's
writev loop would take on a 0. `__stdio_read` does one `SYS_read`
straight into the caller's buffer, dropping musl's readahead-into-`f->buf`
— throughput, not semantics.

## Data structures

None. The seam is macros, four inline functions, and one decode.

## Concurrency

None of its own. The guards are pure; `__syscall_ret` writes only
`errno` (TLS).

## Invariants enforced

pouch's own four (POUCH-DESIGN §11) — no §28 invariant binds this
surface, which is recorded honestly rather than papered over:

- **P-1** structurally, by the sentinel short-circuit on both syscall
  paths.
- **P-3** structurally for un-retargeted calls (`ENOSYS`); by
  construction for retargeted ones (each lower-half patch owns its own
  errno fidelity).
- **P-4** by the UPPER/LOWER/SEAM inventory in
  `docs/reference/78-pouch.md`: a patch that must touch an UPPER entry is
  by definition off the boundary line.

## Error paths

`-ENOSYS` (sentinel, both paths). `EIO` for every flat kernel `-1` —
design-sanctioned imprecision, not a bug: Thylacine's convention carries
no errno, so a lower-half wrapper that can determine something better
does so itself before reaching the decode. Explicit `-errno` in
`[-4095,-2]` passes through, which is how the stalk-resolved calls
(`SYS_open` and friends, since ER-1) deliver real `ENOENT` / `EACCES`.

## Performance

The guard is one compare on a register already loaded. The stdio
rewrite costs one extra syscall per flush when the stream has both a
pending buffer and new data (musl's single `writev` became two
`SYS_write`s); reads lose the opportunistic readahead.

## Prosecution

- **The seam-check list must grow with the series.** `build_sysroot`
  greps the *generated* `bits/syscall.h` for each expected number plus
  sentinel representatives — the only defense against a re-vendor
  silently losing an entry (which would degrade a working call to
  `ENOSYS`). Two separate audit rounds found the same defect — the list
  not extended for the round's new numbers ([[fnd-threads9b-r1-f5]],
  [[fnd-signals13b-r1-f1]]) — which makes it a *lineage*, not a
  coincidence: any patch adding a number must add it to the check.
- The guard must stay on BOTH paths; a new syscall wrapper that hand-rolls
  `svc` (as `__pouch_pipe` legitimately does for its two-register return —
  [[sub-pouch-process]]) is outside the guard by construction and must
  carry a real number.
- The `-1`-before-range ordering in the decode.
- `.rej` files after the apply loop abort the build ([[fnd-seam-r1-f6]]);
  `patch -t` alone would silently skip an already-applied patch.

## Seams

[[seam-pouch-errno-channel]] — the flat-`-1` → `EIO` collapse, and every
per-call errno approximation built on top of it.

## Caveats

- **`docs/REFERENCE.md`'s pouch row (absorbed) says "seven patches" and
  "Ten pouch binaries"** — the series is 31 patches and the ramfs bakes
  24 pouch binaries. The row was written at sub-chunk 14 and never
  re-counted.
- **`78-pouch.md` (absorbed) carried a caveat asserting the opposite of
  the patch it documents**: "`exit` and `exit_group` both terminate the
  whole process. Both map to `SYS_EXITS`." Since #809 the table maps
  `__NR_exit → 0` (`SYS_EXITS`) and `__NR_exit_group → 60`
  (`SYS_EXIT_GROUP`), and 0001's own header says so in as many words.
- The same doc's "Terminal detection always reports 'not a tty'" caveat
  was retired by 0021 and #55c ([[sub-pouch-tty]]) — the section
  documenting the working `isatty` sits 400 lines below the caveat
  denying it.
- 0011 / 0012 / 0013 (the termination overrides, [[sub-pouch-process]])
  are documented in `86-pouch-stratumd-boot.md` and were never mentioned
  in the pouch reference at all.

## Provenance

[[chg-2026-05-22-p6-syscall-seam]] (0001 + 0002 + the build wiring;
[[adt-seam-r1]] 1 P0) → [[chg-2026-05-25-16b-beta-hw-openat]] (0008, the
hw numbers for Stratum's in-process virtio-blk driver) → the number table
has grown at nearly every subsequent pouch landing.

[[chg-2026-08-15-build-targets]] **dropped `tools/build.sh` from this
dossier's `code:` list.** The claim dated from the original landing ("0001 +
0002 + the build wiring") and had never been paid for: the whole file
mentioned the build script exactly once, in the `code:` line claiming it.
Meanwhile [[sub-substrate-build]] describes the sysroot rebuild, the patch
series application and the staleness checks in full, so nothing was lost.
What it cost while it stood was a false signal — an 841-line churn figure
attributed to a dossier that described none of it, competing for a place at
the head of the sweep queue. Same narrowing as the batch-35 pass, for the
same reason: **traversal is not a sweep, and neither is being built by
something.**
