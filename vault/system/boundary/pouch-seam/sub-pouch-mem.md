---
id: sub-pouch-mem
type: sub
parent: moc-pouch-seam
title: "The memory seam — mmap at the asked prot, mprotect / madvise / MAP_FIXED / partial munmap over RESERVE / PROTECT / DECOMMIT / DETACH"
code:
  - usr/lib/pouch/patches/0003-pouch-mman.patch
  - usr/lib/pouch/patches/0044-pouch-mman-protect-decommit.patch
  - usr/lib/pouch/patches/0045-pouch-main-stack-auxv.patch
  - usr/lib/pouch/patches/0046-pouch-init-tls-mmap.patch
  - usr/pouch-hello/pouch-hello-mem.c
  - usr/pouch-hello/pouch-hello-guard.c
audit: hard
guarded-by: [inv-i12, inv-i32]
validated-by: [prose, gate-smp]
locks: []
design: ["docs/ARCHITECTURE.md", "docs/POUCH-DESIGN.md"]
created: 2026-09-23
updated: 2026-09-24
---
## Purpose

musl's `src/mman/` lower half, retargeted onto the kernel's anonymous-memory
syscalls so that a POSIX program's memory is EXACT on Thylacine: a mapping is
minted at the prot asked for, `mprotect` changes it, `madvise` returns pages,
`MAP_FIXED` over one's own memory discards and re-mints, a partial `munmap`
trims. Until B-1b (patch 0044) this half was the P6-pouch-mem shape (0003):
`mmap` ignored `prot` and always minted RW, `MAP_FIXED` was refused, and
`mprotect` / `madvise` / `mremap` sat at the seam's ENOSYS sentinel — which
mallocng tolerated by construction (`&& errno != ENOSYS`) and which was the
reason a pthread's guard was writable, mallocng's freed slots inside a retained
group kept their pages, and a JavaScript engine's decommit returned nothing.
The bar it serves is the operator's (ARCH 6.5 "Capacity"): a program is never
refused memory while free memory exists, and memory it relinquishes returns to
the system so its footprint shrinks at runtime — on this substrate as on the
native one.

## Contract

Numbers, in `src/internal/_pouch_mman.h` (the 0024 `SYS_thyla_*` idiom — an
internal header, never `bits/syscall.h.in`): `SYS_thyla_burrow_reserve` 124
`(length, prot, align_log2) -> vaddr | -errno`, `SYS_thyla_burrow_protect` 125
`(vaddr, length, prot, flags) -> 0 | -errno`, `SYS_thyla_burrow_decommit` 84
`(vaddr, length) -> 0 | -1`, `SYS_thyla_burrow_detach` 38 `(vaddr, length) ->
0 | -1` (`= __NR_munmap`). `__NR_mmap`, `__NR_mprotect`, `__NR_madvise`,
`__NR_mremap` are the sentinel: three of the four Thylacine numbers differ from
their Linux namesakes in argument SHAPE, so a raw `syscall(SYS_mprotect, ..)`
from a port is a clean ENOSYS and can never be misread (a three-argument
mprotect on 125 would pass x3 as the flags word and a stray 1 would SEAL; a
raw madvise on 84 would DECOMMIT on a hint).

- `mmap(0, len, prot, MAP_ANON|MAP_PRIVATE, -1, 0)` → `RESERVE(len, pr, 0)`,
  `pr` = `prot & (R|W|X)` with W implying R (AArch64 Linux has no write-only
  mapping). `PROT_NONE` mints a reservation that faults until raised;
  `PROT_READ` mints read-only; `PROT_EXEC` is EACCES before any call (I-12,
  never degraded — bytes a process wrote become code only through the JIT
  syscalls). Bits beyond R|W|X are ignored, as Linux's mmap ignores them.
  `len == 0` EINVAL, `len >= PTRDIFF_MAX` ENOMEM, `!MAP_ANON` ENOSYS (no
  file-backed mmap, a Plan 9 conviction — ARCH 6.5).
- `mmap(addr, len, prot, MAP_FIXED|MAP_ANON|..)` → `DECOMMIT(addr, len)` then
  `PROTECT(addr, len, pr, 0)`, returning `addr`: Linux's effect over one's own
  anonymous mapping — fresh zero pages at the new prot — as two native calls.
  It never CREATES a mapping at an address and never replaces a file, text or
  shared mapping: a range not wholly plain anonymous memory of this process is
  ENOMEM, decided before anything is discarded (the kernel's admission pass).
  An unaligned `addr` is EINVAL. `MAP_FIXED_NOREPLACE` is ENOSYS.
- `munmap(addr, len)` → `DETACH(addr, len)`, the range form since B-1a':
  partial and multi-mapping unmaps, holes permitted. `len == 0` and an
  unaligned start are EINVAL before the call; the kernel's own refusals (a
  JIT code alias, a shared-in mapping the range would cut, no VMA slot for a
  middle cut) have no Linux analogue and decode to EIO.
- `mprotect(addr, len, prot)` → `PROTECT(start, end - start, pr, 0)` under
  musl's own page rounding; `prot & ~(R|W|X)` EINVAL (Linux's answer for a bit
  it does not know: `PROT_BTI` / `PROT_MTE` / `PROT_GROWSDOWN` here too), W
  implies R, X passes to the kernel's EACCES, a range that rounds to nothing
  is 0; the kernel's -errno through — EACCES for a raise past the mapping's
  mint-time ceiling, a guard, a shared or a JIT mapping; ENOMEM for a hole.
- `madvise(addr, len, advice)`: an unaligned start EINVAL; `MADV_DONTNEED` /
  `MADV_FREE` → `DECOMMIT(addr, len)` (`len == 0` is 0; the kernel's flat -1
  is ENOMEM — the range is not wholly plain anonymous memory of this process,
  or has a hole); the hints NORMAL / RANDOM / SEQUENTIAL / WILLNEED / HUGEPAGE
  / NOHUGEPAGE / DONTDUMP / DODUMP / COLD / PAGEOUT answer 0 without a syscall;
  any other advice EINVAL. `posix_madvise` routes through `__madvise` and keeps
  POSIX's `POSIX_MADV_DONTNEED` short-circuit (advisory, never a discard).
- `mremap` stays a sentinel: `MAP_FAILED` / ENOSYS, and `realloc` copies.
- `pthread_getattr_np(main thread)` (0045, in [[sub-pouch-thread]]) derives
  the initial stack from the auxv pair `AT_STACK_BASE` (0x5342) /
  `AT_STACK_SIZE` (0x5353) the kernel writes — 8 MiB since B-1b — and refuses
  a kernel that writes neither (ENOSYS).

## Mechanism

**mmap.c** is 0003's text rewritten: the prot word first (so a `MAP_FIXED`
request that asks for X is refused before it discards anything), then the
fixed path, then the plain reserve. `__syscall_ret` decodes the -errno the
kernel returns; the one flat -1 (DECOMMIT's) is mapped to ENOMEM before it
would become the generic EIO.

**What it makes real without a musl change.** `pthread_create`'s guard
(`src/thread/pthread_create.c`, untouched): `__mmap(0, size, PROT_NONE)` then
`__mprotect(map + guard, size - guard, RW)` — the guard is now a real
`PROT_NONE` piece of the stack mapping, `DEFAULT_GUARD_SIZE` 8 KiB, and the
kernel's `/proc/<pid>/maps` shows it as a `---p` row of exactly that size
ending at the thread's `stackaddr`. mallocng's metadata areas (`malloc.c`:
`mmap(PROT_NONE)` then `mprotect(p, pagesize, RW)`) are `PROT_NONE` until
raised, as upstream intends. And `__unmapself.s` (0004) detaches the whole
mapping — guard piece and body — in one range detach.

**mallocng's page return.** `glue.h` `USE_MADV_FREE` is 1 (upstream ships 0):
`free()` of a slot inside a RETAINED group (`free.c`: `g->last_idx != 0`, the
slot spans at least one whole page) `madvise(MADV_FREE)`s the whole pages
inside it, and here that is the decommit — the pages return to the pool and
the process's footprint falls without the group being unmapped. A MADV_FREE
page is one Linux may take at any moment and mallocng rewrites a slot's header
at its next `enframe` and never reads a freed slot's bytes for correctness, so
the eager release is inside the allocator's own contract; the header the free
wrote (`p[-3] = 255`, the double-free marker) can be zeroed with the page when
the slot starts page-aligned, which weakens a double-free *diagnostic* (the
`freed_mask` assertion still catches the double free), exactly as on Linux
under reclaim. Whole-group release was already `munmap`.

**0046, required by the parking.** `src/env/__init_tls.c` issues
`__syscall(SYS_mmap2, 0, libc.tls_size, PROT_READ|PROT_WRITE,
MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)` — Linux's six-argument shape — raw,
whenever the static TLS exceeds `builtin_tls`: musl's one raw mmap caller.
Under 0003 it reached 83, and the kernel's arm has read that shape since Clade
CL-4 (`burrow_lazy_len_from_args`: x0 = 0 with an anonymous-private flags word
and fd -1 takes the length from x1; CL-4 met it as one layer of clang++'s
on-device startup, 1232 B of TLS), so it worked. 0044 parks `__NR_mmap` at the
sentinel — a raw Linux-shaped `mmap` must not reach a number that maps RW
whatever prot it asks — and the raw call would then get -ENOSYS, which musl
deliberately does not check, and copy the TLS image to `(void *)-38` before
`main`. 0046 routes it through `__mmap` with musl's own `a_crash()` on
failure: the libc-side elimination CL-4's round named as the clean one. The
kernel's dual-shape arm stays while a binary linked against a pre-0046 libc
can run (the Clade toolchain is cross-built and staged by its own steps). B-1b
first recorded this caller as a live crash, reading the libc side alone; the
holotype round (F1) read the kernel's arm — [[sub-pouch-seam]] keeps the
lesson.

**A re-vendor.** 0001's awk filter loses `m["mmap"]` (0003 added it) and
keeps `m["munmap"]="38"`; the sysroot's SEAM verification (`tools/build.sh`,
`build_sysroot`) pins `SYS_mmap 0xFFFF`, `SYS_munmap 38` and the four numbers
of `_pouch_mman.h`, so a series that loses 0044 fails the build, not the first
program with TLS.

## Data structures

`src/internal/_pouch_mman.h`: the four numbers, `POUCH_BURROW_PROTECT_SEAL`
(1), `AT_THYLA_STACK_BASE` / `AT_THYLA_STACK_SIZE` (mirrors of `elf.h`'s
private tags, pinned by `/pouch-hello-threads` against the kernel's maps row
rather than by a static assert across trees).

## Concurrency

None of its own: every call is one or two syscalls, and the kernel serialises
them under the address space's lock. `MAP_FIXED` is two calls with a window
between them: a concurrent thread of the process can observe the range
decommitted (zero pages) at its old prot, and a refusal of the second call —
a range sealed below the prot asked (EACCES), no VMA slot left for a middle
cut (ENOMEM) — leaves it that way, after the discard (holotype F3; 0044's
header says so). An atomic discard-and-protect needs a kernel flag on
`SYS_BURROW_PROTECT` — an ABI extension, tracked for the operator.

## Invariants enforced

- **I-12** — X is never minted (`PROT_EXEC` is EACCES in `mmap`) and never
  raised (`mprotect(RX)` passes X to the kernel, whose answer is EACCES);
  `pouch-hello-mem`'s x-refused leg pins both.
- **I-32** — memory a program relinquishes returns: `madvise` releases pages
  and the data view of `/proc/<pid>/status` falls (madvise-dontneed /
  madvise-free legs: 1038 pages touched, back to the baseline); mallocng's
  retained-group path returns pages (the mallocng-trim leg: 400 blocks of
  40000 bytes, 4263 pages full, 2339 with every other block freed, 11 with
  all freed — measured 2026-09-23, identical at both smp counts).
- **P-1 .. P-4** (POUCH-DESIGN 11) — every file touched is where musl meets
  the kernel (`mman/`, mallocng's OS glue `glue.h`, the startup path
  `__init_tls.c`); the algorithms above are musl's.

## Error paths

Linux's errnos where the seam can produce them; the divergences, documented:
a HINT on an unmapped range is 0 here (no syscall) where Linux answers
ENOMEM. `munmap`'s kernel-side refusals are EIO (no analogue). A `MAP_FIXED`
over a range that is not wholly this process's plain anonymous memory is
ENOMEM with nothing discarded; a refusal of its second call comes after the
discard (Concurrency). A release over a range with a hole is ENOMEM with
nothing released, where Linux releases the mapped parts first.

## Performance

One syscall per `mmap` / `mprotect` / `madvise(release)` / `munmap`; hints
cost nothing; `MAP_FIXED` costs two. mallocng's `MADV_FREE` is one decommit
per freed slot of a retained group, walking present pagemap slots only.

The eager release has a cost Linux does not pay (holotype F4): each free of a
page-spanning slot in a retained group is a DECOMMIT — a range PTE clear with
a broadcast TLBI, the pages and emptied pagemap nodes freed — and the slot's
next `malloc` re-faults and zeroes each page; mallocng also issues it for the
last slot of a group it is about to unmap. Linux's `MADV_FREE` is lazy (the
page stays until memory pressure, and a write cancels the free), so a
page-sized alloc/free churn costs it nothing. The heritage answer is a
lazy-free list the pool reclaims under pressure (the `capacity_set_reclaim`
hook the image cache already uses); skipping the release for a group about to
go would be a `free.c` change, which P-4 forbids. Unmeasured: a churn
workload — tracked with the B-1 arc.

## Prosecution

- The prot normalisation: W implies R before the X check, so `PROT_WRITE |
  PROT_EXEC` is EACCES, never a silent RW.
- `MAP_FIXED`'s two steps and the state between them; a range straddling a
  hole is refused before the first page goes (`burrow_decommit_in`'s
  admission pass, [[sub-kernel-burrow]]).
- The madvise wrapper's single errno for the kernel's flat -1 (ENOMEM covers
  "a hole", "not anonymous", "below the window" alike — the native 84 keeps
  0 / -1; the phenotype row has the -errno core).
- The guard as two VMAs under `__unmapself`'s one detach (the range form).
- `USE_MADV_FREE` against mallocng's header bytes when a slot starts on a
  page boundary (above).
- `__NR_mmap` at the sentinel: no raw caller of 83 remains — after the
  series `grep -rn 'SYS_mmap\b\|SYS_mmap2\|SYS_mprotect\|SYS_madvise' src`
  finds comments only (one each in `_pouch_mman.h`, `__init_tls.c` and
  `mprotect.c`); the control is `SYS_munmap` in `munmap.c` and
  `__unmapself.c`, which must appear, and did.
- A claimed boundary defect is read from BOTH sides (holotype F1): the
  kernel's arm for a number is part of the seam.

## Seams

None open. [[seam-pouch-guard-pages]] closed at B-1b.

## Caveats

- Hints answer 0 on an unmapped range (Linux: ENOMEM).
- A release over a range with a hole releases nothing (Linux releases the
  mapped parts, then answers ENOMEM).
- `mremap` is a copy fallback; a `realloc` of a large block copies.
- `MAP_FIXED` is two calls: a refusal of the second (a sealed range, no VMA
  slot for a middle cut) comes after the discard and leaves zero pages at the
  old prot (Concurrency).
- `PROT_GROWSDOWN` / `PROT_GROWSUP` / `PROT_BTI` / `PROT_MTE` are EINVAL
  (Linux serves the first two).
- File-backed `mmap` stays ENOSYS by design.
- `MAP_SHARED | MAP_ANON` mints the same private reservation as `MAP_PRIVATE`:
  Pouch has no `fork` (0026, [[sub-pouch-process]]), so within one Pouch
  process the two are indistinguishable, and a `posix_spawn` child shares
  nothing.

## Tests

`/pouch-hello-mem` (joey, boot-fatal, matched on `POUCH_CENSUS_MEM`): twelve
legs — prot-ladder, x-refused, prot-errnos, madvise-dontneed, madvise-free,
madvise-hint, madvise-errnos, partial-munmap, aligned-reserve (WebKit's
`tryReserveUncommittedAligned`: reserve size + alignment, trim both slack
ends, commit an aligned piece), map-fixed (a guard placed by address; a hole
ENOMEM; `MAP_FIXED_NOREPLACE` ENOSYS), mallocng-trim, tls (16 KiB of
`__thread` data written in main and in a thread). Every ENOMEM-on-a-hole leg
is gated by a control that the hole (the top GiB of the burrow window, which
the first-fit allocator reaches last) is absent from `/proc/<pid>/maps`.
`/pouch-hello-guard` (joey, `pouch_smoke_one_expect_fault`): a worker writes
`stackaddr[0]` before it prints its marker (a boundary reported low faults
there and the marker never appears — the positive control, observable since
holotype F2), then `stackaddr[-1]`, and dies of `snare:segv`. `/pouch-hello-threads` ([[sub-pouch-thread]]): main-stack-8mib,
deep-frame, guard-vma. Seven sabotages, each scored by the lines that went red
in its own boot log (recorded on the B-1b audit-trigger row; joey stops at the
first red prover, so a prover behind it in joey's order is not observed in the
same boot): `USE_MADV_FREE 0` → mallocng-trim (half-freed 4023, a fall of 240
from the single-slot groups' unmaps); the release at the sentinel →
madvise-dontneed / -free / -errnos (ENOSYS where ENOMEM is required) and
mallocng-trim; `mmap` minting RW whatever the prot → mem's x-refused
(`mmap(RX)` returned a mapping); `PROT_NONE` minted RW → threads' guard-vma
(`rw-p` where `---p` is required; the guard child, behind it in joey's order,
was not reached); the auxv pair mis-tagged (kernel) → `exec.setup_auxv`, the
suite fails before joey; 0046 dropped from the series (0044 without it) →
mem exits non-zero with no output (the raw call meets the sentinel: a snare
before `main`); the phenotype arm answering 0 without
the core (kernel) → pheno-probe L23l (the page kept its bytes). At the holotype
close, an eighth: `guardlow` (the guard prover's boundary moved one page down)
→ the child dies of `snare:segv` at 0x100003000 before its marker, joey reports
the marker absent and the boot extincts.

## Provenance

[[chg-2026-05-23-p6-mem-b]] (0003: mallocng over the burrow syscalls;
retargeted to `SYS_BURROW_ATTACH_LAZY` at #321) →
[[chg-2026-09-23-b1b-pouch-memory]] (0044 / 0045 / 0046: the seam made
exact; `__init_tls` through `__mmap`; the provers; holotype r1 0 / 0 / 1 / 5).
