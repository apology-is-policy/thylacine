# 23 — Kernel direct map (P3-Bb) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-directmap-doc-absorb`).
The linear PA→KVA mapping in TTBR1's high half (`KERNEL_DIRECT_MAP_BASE`) that
lets the kernel allocator hand back pointers dereferenceable without depending on
TTBR0 — the P3-Bb refactor that made replacing TTBR0 with a per-Proc root safe. A
foundational Phase-3 doc, comprehensively superseded; its content is carried by:

- the **direct-map + vmalloc mapping machinery** — the TTBR1 high-half layout, the
  direct map (every physical page at a fixed KVA by linear offset) and the vmalloc
  range (page-grain device mappings), the identity-map retirement that this
  refactor unblocked (a stray "PA used as VA" now faults loudly), the direct-map
  PTE attributes, the alias semantics, and the TLB visibility:

      vault/system/kernel/memory/sub-kernel-mmu.md

- the **`pa_to_kva` / `kva_to_pa` round-trip and the direct-map cap** — the
  `page.h` conversions, `KP_ZERO`'s zero-via-the-direct-map, and the crucial
  as-built caveat this Phase-3 doc predates: the `l1_directmap[1..8]` cap reaches
  PA [1 GiB, 9 GiB) and is **absolute**, coinciding with QEMU virt's layout only
  because `mem_base == 1 GiB` (#808 / `fnd-808-f2` / `seam-mm-directmap-cap-absolute`):

      vault/system/kernel/memory/sub-kernel-mm-phys.md

- the **allocator consumers that return direct-map pointers** — SLUB's slab
  pointers live in the direct map:

      vault/system/kernel/memory/sub-kernel-mm-slub.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Its P3-Bb forward-references all landed.** "The foundational refactor for
  per-Proc TTBR0 (P3-Bc/Bd)", "Unaffected at P3-Bb (deferred to P3-Bc/Bd)" — the
  per-Proc TTBR0 root, the context-switch swap, and demand paging are all built
  (see `docs/reference/24-per-proc-pgtable.md`, itself absorbed).
- **It predates the #808 direct-map-cap finding.** Nothing here warns that the
  `l1_directmap` cap is absolute rather than relative to `mem_base`; a bringup at
  `mem_base != 1 GiB` would dereference `pa_to_kva(pa)` past the mapped window.
  `sub-kernel-mm-phys` carries the seam and the finding.
