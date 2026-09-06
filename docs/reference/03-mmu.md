# 03 — MMU + W^X [ABSORBED INTO THE VAULT]

This document was absorbed into the vault at the docs/reference retirement
(`chg-2026-09-06-docs-reference-retirement-flip`; the first file absorbed under
the routing flip). Its content now lives, code-verified and current, in the
dossier:

    vault/system/kernel/memory/sub-kernel-mmu.md

(the three kernel views + one user view, the W^X permission encoding and its
seven `_Static_assert`s [I-12], `mmu_enable` / `mmu_program_this_cpu` / the
identity retirement, the two deliberate aliases — the boot-time text patcher and
cross-Proc debug access — neither PTE ever both writable and executable, the
guard-page path and the **#808** boot pre-demote that removes the break-before-make
race *by construction*, the unmap-plus-invalidate that closed the recycled-page
corruption bug, the MMIO vmalloc pool, and the per-Proc trees.)

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- It presents `pte_violates_wxe` as a live runtime check "callable from fault
  handlers." It has **zero callers** (task #59) — a correct predicate nothing
  invokes; the dossier records the real state instead of the aspiration.
- It predates the **Warp-6 V-2 attr-index widening**: `make_user_pte_l3` now
  takes a MAIR index in place of the device bool (adding `NORMAL_NC`
  write-combining for host-visible shared memory), and the W^X extinction was
  widened from *execute-on-device* to *execute-unless-`NORMAL_WB`*.
- It predates the **table-walk coherence correction**: the source once claimed a
  table clean "to the point of coherency" that does not exist and, worse, named an
  instruction that cleans only to *unification* — the walks are sound because they
  are configured cacheable + inner-shareable (the walker participates in
  coherency), not because of any maintenance. A fictional safety argument over a
  true conclusion. (See the dossier's Concurrency section.)
- It predates **I-39** cross-Proc read/write (the debug surface) and the
  rolling-ASID vestigial-`asid`-argument note.
- The exact MAIR / TCR / PTE-bit numeric tables it reproduces live in
  `arch/arm64/mmu.h` — the source of truth — which the dossier points at rather
  than duplicating (a duplicated constant is a constant that rots, which is why
  docs/reference is being retired).
