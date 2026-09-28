---
id: sub-kernel-protect-witness
type: sub
title: "The permission-ceiling and capacity witnesses -- the in-kernel suite, the EL0 probes, and the expect-fault guard child"
parent: moc-kernel-memory
code:
  - kernel/test/test_protect.c
  - usr/protect-probe/src/main.rs
  - usr/protect-probe/Cargo.toml
  - usr/protect-guard-child/src/main.rs
  - usr/protect-guard-child/Cargo.toml
  - usr/capacity-probe/src/main.rs
  - usr/capacity-probe/Cargo.toml
audit: light
guarded-by: [inv-i12, inv-i44, inv-i32]
validated-by: [gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md"]
created: 2026-09-23
updated: 2026-09-23
---
## Purpose

The three witnesses of the permission ceiling (B-1a; ARCH 6.5), owned
together because each sees what the other two cannot. The in-kernel suite
drives the `_for_proc` inners and the fault dispatcher on synthetic Procs and
can look through the page table; the EL0 probe proves the contract as a
program sees it -- a raise really installs on the next touch, bytes survive a
lowering through the user mapping, the errnos come back as `-errno` in `x0`;
and the guard child proves the one thing neither of those can, a REFUSED
access, because that is a `snare:segv` death and only a spawned Proc can die.
Since B-1a' (2026-09-23) the dossier also owns `/capacity-probe`, the EL0 half
of the capacity contract; its in-kernel half, `kernel/test/test_capacity.c`,
is [[sub-kernel-pagemap]]'s.

## Contract

`kernel/test/test_protect.c` registers nineteen tests in `kernel/test/test.c`'s
table: twelve `protect.*`, two `cow.clone_*`, one `sys_burrow.detach_piece_*`
from the chunk (the list, one line each, heads the file; [[sub-kernel-vma]]
carries it too), and four from the holotype audit's close --
`protect.range_walk_is_linear`, `protect.uninstall_range_skips_absent_subtrees`,
`protect.noop_protect_needs_no_headroom`, `sys_mmap.fixed_anon_w_alone_maps_rw`.
The audit's F1 pair, `protect.file_pagein_racing_protect_bails_{single,cluster}`,
lives in `kernel/test/test_demand_page.c` beside the stub Dev it needs: the
stub `dev->read` runs with `as->lock` dropped, so it is the one deterministic
place a sibling's `SYS_BURROW_PROTECT` can be interposed between a FILE
fault's admission and its install ([[sub-kernel-fault]]). They run in the
ordinary suite at `-smp 1` and `-smp 4`.

`/protect-probe` (native libthyla-rs, `usr/protect-probe`) is spawned by joey
after the alloc smoke and reaped by pid; it prints `protect-probe: <leg> OK`
per leg and `protect-probe: ALL OK`, exit 0 -- or `protect-probe: FAIL <leg>`,
exit 1. Eight legs: `reserve-raise-write` (reserve 4 pages at none, raise to
RW, write, read back), `relro-read-only-keeps-bytes` (lower page 0 to R; the
pattern is still there), `grow-shrink-grow-keeps-contents` (the whole range to
none and back to RW; the pattern survives), `seal` (page 2 sealed at R: RW is
EACCES for good, none and R still reachable), `x-never-a-target` (protect(R|X)
over an UNMAPPED range is EACCES where protect(R) is ENOMEM; reserve(R|X) and
protect(R|X) on a live mapping are EACCES), `refusals` (an unaligned address,
W-only, an unknown flag, a zero length -- each EINVAL, the pattern untouched),
`aligned-reserve` (a 2 MiB-aligned reservation), `piece-detach` (the pieces
the protects left -- `[0,2)` RW, `[2,3)` R sealed, `[3,4)` RW -- detach one by
one, and a fresh reserve lands at the first base again).

`/protect-guard-child` (`usr/protect-guard-child`) reserves two pages RW,
writes page 1 to prove the reservation is live memory, seals page 0 at none,
writes `protect-guard-child: touching the guard` on fd 1, and writes through
page 0. The kernel MUST refuse that fault ([[sub-kernel-fault]] step 2, prot
none, before any Burrow is resolved) and terminate the Proc via `snare:segv`.
Surviving prints `SURVIVED` and exits 0.

`/capacity-probe` (native libthyla-rs, `usr/capacity-probe`; B-1a') is spawned
by joey after the guard child and reaped by pid; it prints `capacity-probe:
<leg> OK` per leg and `capacity-probe: ALL OK`, exit 0 -- or `capacity-probe:
FAIL <leg>` (with `got=<n> want=<m>` on a census miss), exit 1. Seven legs,
each asserting the exact data census of `/proc/<pid>/status` -- `pages:`
minus `tables:` minus `file:`, since the round-1 close counts the hardware
page tables in `pages:` and the round-2 close the program's own text and
rodata (the Image cache's pages it maps, charged per leaf touched), both
reported apart ([[sub-kernel-devproc]]); data plus the pagemap's nodes -- against a baseline taken at the start:
`reserve-4gib-touch-census` (a 4 GiB reserve is admitted -- the old cap
refused it -- costs nothing untouched, and touched once every 512 MiB raises
the census by 8 pages + 13 nodes), `protect-middle-r` (the middle GiB to R;
the bytes stay readable through the R piece; nothing released),
`range-detach-across-pieces` (a 2 GiB range across the first piece's tail, the
R piece whole and the last piece's head returns exactly 4 pages + 6 nodes; the
survivors keep their bytes), `footprint-shrinks-to-start` (the rest in one
range over the hole and both survivors: the census is back at its baseline,
and a repeat of the same range answers 0), `lazy-over-256mib-detaches` (a
512 MiB reserve, two pages touched, detaches whole), `eager-pages-go-with-the-last-piece`
(an eager 4-page region middle-cut refunds nothing and keeps its outer bytes;
the two remaining pieces give the block back), and `refusals` (an unaligned
base, a zero length and a range below the window each -1, the census
unmoved).

## Mechanism

**The expect-fault census.** joey reaps the guard child with
`pouch_smoke_one_expect_fault`, which requires BOTH the marker in the child's
stdout pipe AND a non-zero exit status. Either half alone is satisfiable by a
broken system: a child that died before the seal (no marker) or one whose
guard did not guard (marker, exit 0). The marker goes out through `t_write(1,
...)`, not `t_putstr`: `t_putstr` is `SYS_PUTS`, the console, and the census
reads the pipe ([[sub-stratum-boot]] carries joey's side).

**Sabotage-measured, each on its own assertions.** Four REDs were applied one
at a time, one suite boot at `-smp 1` each, then reverted and the clean kernel
rebuilt (a sabotage leaves its kernel in `build/`): the PTE uninstall skipped
(`nouninstall`) -> 1630/1632, failing "the PTE is uninstalled" and "the PTE is
gone"; both X refusals dropped (`noxcheck`) -> 1629/1632, failing "X is never a
mint", "protect(X) over nothing is EACCES" and "X never returns"; the ceiling
compare skipped (`noceiling`) -> 1630/1632, failing "cannot be raised to RW"
and "RW is gone for good"; a clone per VMA again (`nodedupe`) -> 1631/1632,
failing "each page has exactly TWO holders, not one per piece". Nothing else
failed in any RED, and the suite extincts on a failure, so the probes never run
in a RED -- the kernel tests are the discriminating half, the probes the EL0
half.

**The audit's combined RED.** Every fix of the close reverted at once -- the
F1 admission re-check, the four linear walks, the subtree skip, the no-op
short-circuit, the fixed-arm W promotion, the mprotect order -- with the
diagnostics and the tests kept, one suite boot at `-smp 1`: the six new tests
fail and nothing else does. Each test names one fix in its assertions, so
the combined RED shows each can fail; it does not attribute a failure to a
fix the way the chunk's four one-at-a-time REDs did.

**The probe detaches pieces one by one, on purpose.** The native
`SYS_BURROW_DETACH` matches one VMA exactly; the range form is B-1a'. So the
last leg is also the piece-detach witness from EL0, and its final check -- a
fresh page-aligned reserve lands at the base the first one had -- is the
control that the address space is really free again.

**The X literal.** libthyla-rs deliberately has no `T_BURROW_PROT_EXEC`: a
constant nobody may pass is not worth mirroring. The probe spells `X = 4` as a
local literal, only to watch it be refused.

**The census is read, never restated.** `/capacity-probe` opens
`/proc/<pid>/status` with the pid from `t_getpid` -- there is no `/proc/self`;
devproc has the Plan 9 shape -- and reads it once before taking its baseline,
so the pages its own stack needs for a read are resident already and every
later figure is a delta of the reservations alone. Each leg then asserts an
exact value, and a miss prints both figures, so a wrong node count (the map's
metadata is part of the census) fails as loudly as a wrong page count. As
committed at `a1649f92` the probe opened `/proc/self/status` and its first leg
failed at boot; the corrected path is joey's own (`proc_status_field`).

## Data structures

None of its own. The suite's helpers (`mk`/`drop` a Proc, `reserve`, `protect`,
`map_eager`, `fault`, `pte_of`, `count_vmas`, `slot_page`, `adopt`) are file-local.

## Concurrency

The suite runs on the boot CPU against fresh address spaces; nothing here is
concurrent. The SMP gate is what exercises a genuine concurrent break
([[gate-smp]]).

## Invariants enforced

None enforced -- witnessed. [[inv-i12]] (X refused before the lookup and in
the mechanism; RX only descends), [[inv-i44]] (the clone dedupe, the
ceiling-keyed share, the uninstall-before-prot order, a cut COW mapping breaking
soundly), [[inv-i32]] (a detached piece refunds exactly its own pages; the
grow ladder never holds more than two VMAs; nothing leaks across a three-way
split and merge).

## Error paths

The probe's legs each name their failure; the guard child exits 2 on a failed
reserve or seal, which the census reads as a failure (non-zero, but no marker).

## Performance

Negligible: a few pages, a few faults.

## Prosecution

What a reviewer attacks here: an assertion satisfiable by a broken system (the
census needs both halves; the `x-never-a-target` leg needs the ENOMEM control
beside the EACCES, or a refusal that happened for the wrong reason passes); a
leg that reads back the value it just wrote through a mapping that was never
uninstalled (the grow / shrink / grow leg goes through none, so the re-fault is
real); and a RED that fails more than its own assertions (measured: none does).

## Seams

- `usr/viv-pheno-probe/src/main.rs` is UNOWNED (thousands of lines, hundreds
  of legs); B-1a's legs L22 / L22b (the `mprotect` errnos over a never-mapped
  range) and L23..L23h (the reserve-then-commit ladder: `mmap` PROT_NONE,
  `mprotect` RW, write, R, read back, RX -> EACCES, none, a zero length, an
  unaligned address, `munmap`) are described in
  [[sub-kernel-syscall-dispatch]]'s B-1a section, and B-1a''s L21 / L21c /
  L21d (the fixed mapping in the window, its `munmap`, the below-window
  decline) in [[sub-kernel-vivarium]]; the probe's own dossier is owed by the
  sweep.
- The holotype audit of the range detach is owed ([[arc-boosty]]); no RED has
  been recorded against `/capacity-probe` or `test_capacity.c` yet, so the
  capacity half of this dossier is not yet sabotage-measured the way the
  ceiling half is.

## Caveats

The probe's `piece-detach` leg keeps detaching one piece at a time under the
range form (a piece's exact span is a range like any other), so it stays the
piece-detach witness; the range legs are `/capacity-probe`'s. Its final
control -- a fresh reserve lands at the first base again -- is unchanged.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
