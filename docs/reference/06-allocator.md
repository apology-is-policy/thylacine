# 06 — Physical allocator [ABSORBED INTO THE VAULT]

Absorbed at the memory/ipc-wake sweep (`chg-2026-08-01-mm-ipc-sweep`).
Its content now lives, code-verified and current, in:

    vault/system/kernel/memory/sub-kernel-mm-phys.md

(the five-reservation bootstrap, the F3 8 GiB cap and its
relative-vs-absolute loose end, the #808 boot page-map, the buddy,
the #807 per-CPU magazines, the KP_ZERO barrier, the lock lattice.)

**What this file got WRONG by the time it was absorbed.** Frozen at
P1-D (2026-05-04) with exactly ONE current paragraph — the #807
magazine SMP section — stitched into a fourteen-month-old body:

- `struct page` given as **32 bytes** in the struct listing, the
  Data-structures table, and the array math (16 MiB at 2 GiB) —
  48 bytes since P1-E the next day, `_Static_assert`-pinned, the
  array 24 MiB. The listing also omits `PG_SLAB`.
- **Three reservations**, sorted and gap-walked — five since P4-E
  (explicit low-firmware F34 + initrd), with the F29 disjointness
  check absent.
- No F3 cap, no #808 page-map step, no F5 `dsb ish` — the layout
  section ends the zone at the DTB end unconditionally.
- `kpage_alloc` presented as returning PA-as-void* through TTBR0
  identity, with the direct map as "Phase 2 will" — three eras after
  P3-Bb landed it.
- SLUB listed under "Not yet implemented".
- The head claims magazine refill is "batched: each acquisition
  covers 8 pages amortized" while the appended #807 paragraph
  correctly describes ONE buddy-lock acquisition PER PAGE — the
  assert-and-opposite shape, in the very doc whose code
  (`mm/magazines.c`'s header) explicitly corrects the head's claim
  and names the bulk-op as lift HT11.R1-F6.

Two code headers carry the same freeze and are recorded as dossier
caveats for main-track fixing: `mm/phys.h` (the P1-D PA-as-void*
comment) and `kernel/include/thylacine/page.h` (`KP_NOWAIT` —
"implicit at v1.0 (no scheduler)").
