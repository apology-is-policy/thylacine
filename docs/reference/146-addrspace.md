# 146 — AddrSpace (the shared address space) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-vma-addrspace-absorb`;
the memory area). Its content now lives, code-verified and current, in the
dossier:

    vault/system/kernel/memory/sub-kernel-addrspace.md

(the translation-vs-process membership test, the atomic-from-day-one refcount,
the drain-at-last-reference fix [L-3], the no-TLB-flush-at-teardown argument
resting on the non-global ASID tag, the three-phase COW clone and why the
parent's PTE uninstall must precede the share, the per-page `cow_share` count
and why it lives on the page not the slot, the six I-32 CAS-loop counters and
the floor-not-accountant tolerance, the mmu-API-stayed-bare rationale, the
grep-rename footgun, and the `test_addrspace.c` suite. I-44, I-32, I-31, I-12,
I-5/I-34.)

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- It is an **L-1-only** account: it describes the `struct AddrSpace` extraction
  and the refcount, but predates the rest of the LINEAGE arc — the copy-on-write
  break (`cow.c`, the per-page `cow_share` count, the single-step decide), the
  `addrspace_clone` fork, and the L-3 drain-at-last-reference fix — all of which
  the dossier carries.
- Its `addrspace_alloc(void)` signature and "NULL on OOM" contract predate the
  overcommit budget: the entry point is now `addrspace_alloc(page_budget)` and
  refuses a **0 budget** (an uncapped address space is the I-32 DoS hole),
  with `page_budget` living beside the count it bounds.
- The exact `struct AddrSpace` / `struct page.cow_share` byte layouts live in
  `kernel/include/thylacine/addrspace.h` and the page struct — the source of
  truth — which the dossier points at rather than duplicating.
