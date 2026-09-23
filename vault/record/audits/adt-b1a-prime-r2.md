---
id: adt-b1a-prime-r2
type: adt
title: "B-1a' (capacity) round 2: the Image cache was a pool hoard nobody paid for, and the copy-on-write break tore its tables down and rebuilt them against a pool it held no lock on"
date: 2026-09-23
scope: [sub-kernel-image, sub-kernel-mm-phys, sub-kernel-mmu, sub-kernel-fault, sub-kernel-addrspace, sub-kernel-vma, sub-kernel-burrow, sub-kernel-devproc, spec-capacity]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 0, p2: 2, p3: 2}
findings: [fnd-b1a-prime-r2-f8, fnd-b1a-prime-r2-f9]
round-of: chg-2026-09-23-b1a-prime-close
created: 2026-09-23
---
## Scope

Branch `b1a-prime-wip` at d04189fb (the round-1 close's code: the physical
pool, the hardware page tables charged and reclaimed, F3 / F4 / F6 / F7) plus
a two-line dedup in `arch/arm64/mmu.c`. Read-only. The brief asked for the
one return site (every path a `PG_USER` page can leave the allocator), the
table reclaim's TLBI argument and its software walkers, the occupancy count's
every writer, the charge order on the fault path, the F4 record and the F3
guard, the exempt plumbing (whose exemption pays at each creator and arm, and
whether the FILE arm's `as = NULL` counts anywhere), the tests' physical
controls, the data-view ABI, netd's oracle, and whether the close's
restructuring voided any round-1 withdrawal.

## Convergence

0 P0 / 0 P1 / 2 P2 / 2 P3 -- not dirty by count, dirty by the shape of the
fixes (a reclaim hook inside the page allocator, a holder charge on pages the
holder does not own, a third leaf writer, a durable linked-but-empty table
state), so a round 3 on the fixes follows before the SMP gate. F8
([[fnd-b1a-prime-r2-f8]]) is the finding of the round and a design hole the
physical pool exposed: the FILE page-in took a pool page and installed it
against no address space, so a dead Proc's images stayed pool-charged as idle
cache entries that nothing reclaimed until 120 more distinct images evicted
them -- every later user allocation refused while free-able memory existed --
and a confined Proc never saw its own text on its cap. Attribution: the
uncharged FILE posture predates the chunk and was worse (unbounded); the
physical pool turned "eat all RAM" into "hold the whole pool after death".
F9 ([[fnd-b1a-prime-r2-f9]]) prosecuted the round-1 close itself: the
copy-on-write break's unconditional uninstall, load-bearing under the old
allocator, freed the leaf's tables under the new one and the re-install
re-allocated them through a pool charge that respects no lock the faulter
holds -- a pool-edge termination race, and a table churn per break. F10 [P3]:
a pool refusal at `SYS_BURROW_ATTACH` and `SYS_LOOM_SETUP` surfaced as `-1`
(the pouch EIO sentinel) while the cap refusal beside it said ENOMEM. F11
[P3]: two `devproc.c` comments called `page_count` the anon count and the
peak anon commit after tables (and now file pages) joined it; the manual text
and a `/ctl/procs` column stay with the prowl telemetry sub-chunk. All four
fixed before landing ([[chg-2026-09-23-b1a-prime-close-r2]]), the two P2s by
heritage shapes auto-accepted under the operator-away grant: Plan 9's
`imagereclaim()` when the page pool runs low and Linux's page cache giving
way to anonymous demand with memcg charging the page cache to the toucher
(F8); a break-before-make on the leaf, and the parent's tables kept across a
fork as Linux keeps them (F9). Withdrawn as guarded: tail pages of an order>0
block, a stale `PG_USER` after recycling, the L0 in the pool, the reclaim
freeing a pre-existing table at occupancy 0 (VOIDED by keep-tables; re-opened
for round 3), the parent occupancy underflow, the software walkers, the
uncharge clamp, the F4 residue, the F3 guard against shares, the netd oracle.
Test gaps named, not findings: a reclaimed table page physically zero; a table
charge refused at the L2 / L3 level; an SMP fault racing a reclaim; FILE pages
returning at eviction. The verbatim report and dispositions are the repo's
untracked `memory/audit_b1a_prime_closed_list.md`.
