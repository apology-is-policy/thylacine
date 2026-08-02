---
id: inv-i32
type: inv
title: "I-32 — the per-Proc resource floor"
number: I-32
guards: [sub-kernel-proc, sub-kernel-hwcap, sub-kernel-content]
validated-by: [gate-smp]
strength: prose
created: 2026-08-01
updated: 2026-08-02
---
## Statement

A non-TCB Proc's resource use is bounded on every axis a hostile or buggy
program can drive, so a fork bomb / thread bomb / memory bomb hits a clean
per-Proc limit instead of stressing the allocator toward the box-killing
cliff. On a bound, creation fails cleanly (`-ENOMEM` / `-EAGAIN` / a refused
fault) — it never box-extincts.

This is a **resource** axis, deliberately orthogonal to authority: it is not
a privilege gate and it composes with, rather than substitutes for, the
capability model.

## Enforcement

Five axes, all counted on `struct Proc` and all defined in
`kernel/include/thylacine/proc.h`:

| Axis | Counter | Cap | Charged under |
|---|---|---|---|
| anon pages | `page_count` (+ `page_peak`) | `page_budget`, seeded `PROC_PAGE_MAX`, ≤ `PROC_PAGE_HARD_MAX` | `p->vma_lock` — **exact** |
| live VMAs | `vma_count` | `PROC_VMA_MAX` | `p->vma_lock` — **exact** |
| shared-in pages | `shared_map_pages` | `PROC_SHARED_MAP_MAX_PAGES` | `p->vma_lock` — **exact** |
| direct children | `child_count` | `PROC_CHILD_MAX` | [[lock-proc-table]] — bounded overshoot |
| live threads | `thread_count` | `PROC_THREAD_MAX` | [[lock-proc-table]] — bounded overshoot |

The three `vma_lock` axes are exact because check-and-charge happen inside
the lock that already serializes the attach/detach path. The two creation
gates read under the table lock and increment at a *later* hold, so they
carry a TOCTOU overshoot of at most `ncpus-1` concurrent spawners — stated
in the code as acceptable **for a floor**: a bound, not an accountant.

`bounce_bytes` (the transient byte-I/O staging heap) is an I-32-shaped sixth
axis with a softer failure mode — over budget *degrades* to the stack tier,
producing a short op rather than a failed one.

**DMA buffers are on none of these axes.** A driver's DMA pages come from the
same allocator as anon pages but are charged to no counter, so `page_count` is
not the true page footprint of any Proc holding hardware. The bound is elsewhere
and differently shaped: the allowance's **per-buffer** ceiling
([[sub-kernel-hwcap]]), which caps one buffer rather than their sum. That shape
is structural rather than accidental — [[inv-i34]]'s conferred set carries a
single maximum size, so there is nowhere in its data model for a sum to live,
and a cumulative budget would have to extend the model rather than add a check.
It is a recorded future item. The gap is narrow today because the
capability to create a DMA buffer is itself tightly held — so the bound is on
*who may ask*, not on how much they may accumulate, which is a different kind of
floor from the rest of this invariant and worth stating rather than assuming.

**The exemption is the load-bearing part.** `proc_resource_exempt` is
`principal_id == PRINCIPAL_SYSTEM` — the TCB, so the floor cannot pinch the
FS server, the orphan-adopter, or the kthread root. It is unforgeable
because `CAP_SET_IDENTITY` refuses to stamp `PRINCIPAL_SYSTEM` (and
`proc_apply_identity` extincts on the attempt), and `principal_id` is
immutable on a running Proc — so a plain read is sound. A NULL Proc reads
non-exempt (fail-closed).

The **graceful-OOM backstop** is what bounds the recursive case: every user
creation path (`proc_alloc`, `thread_create`, `territory_clone`,
`burrow_create_anon`, the demand-page install) returns an error or
per-Proc-terminates, never a box extinction. So a bomb that evades a
per-Proc cap by spreading across Procs still terminates at the physical
cliff instead of taking the machine.

**Subsystem-local bounds are the same invariant at a smaller scale, and they are
not on this table.** The per-Proc environment ([[sub-kernel-content]]) caps its
variable count and each value's length, which bounds a hostile program's kernel
allocation there to a quarter-megabyte per Proc. These are not counted on
`struct Proc` and never reach the axes above — they are enforced where the
allocation happens, by the structure that owns it. That is the general shape:
this invariant's table lists the axes with a *shared* allocator behind them, and
each subsystem that allocates on a Proc's behalf carries its own ceiling. A
reader auditing "is this Proc bounded" must therefore consult both, and the table
alone will read as more complete than it is.

`page_budget` (CL-5) makes the page axis per-Proc rather than global:
inherited across `rfork` (load-bearing — `make` and `clang` are Pouch ports
that know nothing of budgets, so only inheritance carries a raise from the
build root down to `cc1`), freely *lowered* by any spawner (monotonic
reduction, the I-2 shape), raised only with
`PROC_FLAG_MAY_RAISE_PAGE_BUDGET`, and never above the hard cap by any
authority.

## Validation

Prose + the focused audits; [[gate-smp]] for the counter races. **blind-to:**
there is no global or per-user aggregate — the counters vanish with the Proc,
so a cgroup-equivalent reading them is a recorded seam. The two creation
gates' overshoot is real and deliberate. `page_peak` is telemetry only; no
policy reads it.
