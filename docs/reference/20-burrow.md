# 20 — Burrow (the memory object) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-burrow-absorb`; the
memory area). Its content lives across the dossiers that own each piece:

- the Burrow itself — the dual refcount and why one counter cannot express
  liveness, the six backing types and eager-vs-sparse lifetimes, the free
  decision crossing the unlock exactly once, `burrow_backing_pages` occupancy
  charging, the charge-record attribution, the cross-Proc `burrow_share_into`
  (whole-region) and the two kernel-minted DMA subtype bits, the deferred-free
  burrow-side API (`burrow_release_mapping_deferred` / `burrow_free_deferred` /
  `burrow_map_fixed`), and I-7/I-32:

      vault/system/kernel/memory/sub-kernel-burrow.md

- the **handle-table integration** (`handle_release_obj`'s `KOBJ_BURROW` case
  dropping the ref *outside* the table lock because the free may sleep, the
  `burrow_create_anon`-consumed-reference convention, the TRANSFERABLE class):

      vault/system/kernel/security/sub-kernel-handle.md

- the **unmap PTE-teardown-before-free ordering** — `vma_free` calls
  `mmu_uninstall_user_range` + `tlbi` *before* the backing pages return to the
  buddy, the AEGIS-256/mallocng stale-mapping corruption class it closes — now
  folded into:

      vault/system/kernel/memory/sub-kernel-vma.md

The per-page bit-47 VA reject (the demand-page-path defense-in-depth beyond the
VMA-layer ceiling) lives in `mmu_install_user_pte` (`arch/arm64/mmu.c`) — the
source of truth for the bound; sub-kernel-burrow carries the VMA-layer ceiling
check it duplicates.

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- Its own opening summary **contradicts its own enum**: the preamble says the
  backing type is "`BURROW_TYPE_ANON` at v1.0; PHYS at Phase 3; FILE post-v1.0"
  and, lines later, "At v1.0: `BURROW_TYPE_ANON` only" — while the enum below
  defines **six** types (the two sparse backings and the executable-memory one
  included). An opening summary decays fastest: every author edits the line they
  are changing, and nobody's change is about the preamble. The dossier carries
  all six.
- Its `burrow_share_into` section is marked "Scope: ANON only"; the share is no
  longer anon-only (ANON plus the kernel-minted `weave` / `gpu_bo` DMA subtype
  bits, each with its own hardware-isolation argument on its own field).
- Its exact `struct Burrow` field layout and the enum values live in
  `kernel/include/thylacine/burrow.h` — the source of truth — which the dossier
  points at rather than duplicating.
