# 79 — SYS_BURROW_ATTACH / SYS_BURROW_DETACH (P6-pouch-mem-a) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-sys-burrow-doc-absorb`).
The v1.0 native anonymous-memory interface (Tier 1): `SYS_BURROW_ATTACH` (37)
attaches an anonymous RW demand-zero Burrow at a kernel-chosen VA;
`SYS_BURROW_DETACH` (38) tears one down. The substrate for `malloc`. Its content
lives, code-verified and current, in:

- the **Tier-1 anon Burrow + the I-7 dual-refcount discipline** — the attach dance
  (`burrow_create_anon` at `handle_count=1`, `burrow_map` takes `mapping_count->1`,
  `burrow_unref` drops the construction handle to 0 while the mapping keeps it
  alive), the eager power-of-two backing, the free-when-both-zero rule:

      vault/system/kernel/memory/sub-kernel-burrow.md   (audit: hard, inv-i7)

- **`vma_find_gap`** (the overflow-free first-fit that never forms `cand+length`),
  the detach path, and the **P6-pouch-mem-a F1 window-confinement finding folded
  here at this absorption**:

      vault/system/kernel/memory/sub-kernel-vma.md   (audit: hard, inv-i1/inv-i12)

- the **per-AddrSpace VMA lock** and the attach/share concurrency (a sibling
  thread's `SYS_BURROW_ATTACH` racing the same list):

      vault/system/kernel/memory/sub-kernel-addrspace.md

- the **handlers + ABI** — the thin `current_thread()` wrappers over the testable
  `_for_proc` inners, `BURROW_ATTACH_MAX` (256 MiB), the burrow-attach window
  (`EXEC_USER_BURROW_BASE` 4 GiB / `TOP` 64 TiB, above the stack):

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- the **pouch translation** — `mmap(MAP_ANONYMOUS)`/`munmap` onto these two
  syscalls (the `0003-pouch-mman` seam):

      vault/system/boundary/pouch-seam/sub-pouch-process.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The F1 window-confinement finding was uncovered — folded at absorption.**
  `vma_find_gap` (overflow-free), the anon refcount dance (I-7), and the vma_lock
  concurrency were all home, but the security finding — that `burrow_unmap` matches
  a VMA by geometry alone, so `SYS_BURROW_DETACH` must reject any vaddr outside the
  burrow window *before* the match, or EL0 could name its own ELF/stack/stack-guard
  VMA and have it dismantled (the guard case silently retiring a security page) —
  lived only here. Now folded into the sub-kernel-vma Prosecution, tied to I-1.
- **The two-tier model + the deliberate refusals are current** — Tier 1 (anon, no
  handle, VMA-owned) vs Tier 2 (handle-backed, deferred); `brk` refused
  (ASLR/thread/arena-hostile); file-backed `mmap` refused (not 9P-transparent). The
  `libt` wrappers remain deferred (pouch issues the syscalls directly). Zero code
  change.
