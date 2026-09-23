---
id: adt-b1a-prime-r1
type: adt
title: "B-1a' (capacity) round 1: the reserve was a fiction -- uncharged, death-reclaimed page tables -- and a holder-counted pool refused a fork with free memory"
date: 2026-09-23
scope: [sub-kernel-pagemap, sub-kernel-vma, sub-kernel-burrow, sub-kernel-fault, sub-kernel-addrspace, sub-kernel-mmu, sub-kernel-mm-phys, sub-netd-server, spec-capacity]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 2, p2: 0, p3: 5}
findings: [fnd-b1a-prime-r1-f1, fnd-b1a-prime-r1-f2]
round-of: chg-2026-09-23-b1a-prime-capacity
created: 2026-09-23
---
## Scope

Branch `b1a-prime-wip` at a1649f92 plus the working tree that became c35ec420
(WIP 4): the charged radix pagemap, the range detach core, the user pool and the
I-32 default, the window-confined fixed arms, `specs/capacity.tla`, the twelve
tests, the EL0 probes. Read-only. The brief asked for the accounting (every
charge paired on every path), the concurrency (a fault racing a detach, the COW
break racing a take, the clone racing a sibling's fault), the lifetime of pagemap
nodes handed back after the unlock, the walk bounds at depth 4, refusal
completeness between phase 1 and phase 3, the FILE arm's uncharged partial
detach, the fixed arms and the window, and which of the twelve assertions could
not fail.

## Convergence

Dirty by count (0 P0 / 2 P1 / 0 P2 / 5 P3): a round 2 on the fixes follows.
F2 the reviewer found independently of the chunk's own self-audit, which had
already fixed it ([[fnd-b1a-prime-r1-f2]]) -- the coverage signal the discipline
asks for. F1 ([[fnd-b1a-prime-r1-f1]]) is the finding of the day and a SYSTEM
finding: uncharged, death-reclaimed hardware page tables let an unprivileged Proc
empty the buddy at a charged count of three; they predate the chunk, and the
chunk's scripture claim ("the reserve keeps `PRINCIPAL_SYSTEM` allocating")
rested on them. The five P3s were a release loop on an unclamped bound (F3), the
eager charge record keyed on a pid that survives exec (F4), a design tension --
a holder-counted pool refusing a fork of a Proc holding more than half the room
while free memory existed, against the ratified bar (F5) -- netd's retirement
oracle lowering a live ring on its failure path (F6), and a test comparing the
reserve formula with itself (F7). All seven fixed before landing; F5 and F1's
return path by one move, the pool made PHYSICAL (charged at `alloc_user_pages`,
returned at `free_pages`; [[sub-kernel-mm-phys]]), which is the heritage shape
(Linux memcg: the charge on the page, the cap on the holder) and was
auto-accepted under the operator-away grant. Withdrawn as guarded: the
racer-built path's node pairing, a fault racing a range detach, the COW break
against a take, the death return against a late uncharge, a middle cut at the
cap, MAP_FIXED over every shape, the 64 TiB arithmetic. Test gaps named, not
findings: no guard-VMA middle cut, no FILE partial detach through the core, no
MMIO/DMA trim, no concurrent fault-vs-detach at -smp, no exempt-space headroom
bypass. The verbatim report and dispositions are the repo's untracked
`memory/audit_b1a_prime_closed_list.md`.
