# 26 — VMA (virtual memory areas) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-vma-addrspace-absorb`;
the memory area). Its content now lives, code-verified and current, in the
dossier:

    vault/system/kernel/memory/sub-kernel-vma.md

(the four `vma_alloc` rejections and why `WRITE|EXEC` is the single user-side
W^X gate, the guard-VMA exception, the sorted-list mechanism and the half-open
overlap semantic [`a < d && c < b`; adjacent ranges are not overlap], the
overflow-free `vma_find_gap` arithmetic, the DISTRO D-3 MAP_FIXED
split/replace [`vma_replace_range_in`] and its hole-free failure paths, the
COW flag as routing-not-truth, and the six-test `test_vma.c` suite. I-12, I-7,
I-32, I-44.)

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- It predates the **LINEAGE address-space extraction**: it describes the VMA
  list as hanging off `struct Proc`, but the list moved to `struct AddrSpace`
  (so two `RFMEM`-sharing Procs see *one* list, not two views), and `lock-vma`
  moved with it. A reader following this doc would add a lock to the wrong
  structure — precisely the trap the dossier's Concurrency section calls out
  (task #60: the `.h` docblock is still stale in-tree).
- It predates the **copy-on-write flag** (`VMA_FLAG_COW`, routing-not-truth, the
  permissions-disagree-with-hardware inversion) and the **DISTRO D-3 file-mmap
  surface** (`vma_replace_range_in`, `vma_next_overlap_in`, and
  `vma_free_deferred` with the sleeping-free-under-lock deferral for a FILE
  Burrow).
- Its concurrency posture ("v1.0 single-thread-Proc, no new concurrency") is
  superseded by the **#713 `vma_lock` coverage** — the fault handler is a reader
  that now holds the same lock, closing the half-unlinked-list UAF.
- Its exact `struct Vma` byte layout and the `ranges_overlap` form live in
  `kernel/include/thylacine/vma.h` / `kernel/vma.c` — the source of truth — which
  the dossier points at rather than duplicating.
