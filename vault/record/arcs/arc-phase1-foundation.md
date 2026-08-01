---
id: arc-phase1-foundation
type: arc
title: "Phase 1: the foundation — allocators under the freshly-lit MMU"
status: complete
design: ["docs/ARCHITECTURE.md 6.3", "docs/ARCHITECTURE.md 6.4"]
chunks:
  - chg-2026-05-04-p1d-phys-allocator
  - chg-2026-05-04-p1e-slub
  - chg-2026-05-05-p1id-closing-audit
follow-ons: [seam-buddy-bulk-op, seam-slub-debug-mode]
created: 2026-08-01
---
## Goal

The Phase-1 memory slice as it matters to the vault's batch-9 areas:
pages (buddy + magazines + the DTB-driven bootstrap), then objects
(SLUB), then the closing audit that hardened both. The wider Phase 1
(boot, MMU, KASLR, exceptions) belongs to the boot/entry areas.

## Shape

- **P1-D** ([[chg-2026-05-04-p1d-phys-allocator]]) — buddy + magazines
  + phys bootstrap, single-CPU, spinlock stubs.
- **P1-E** ([[chg-2026-05-04-p1e-slub]]) — SLUB over it; `struct page`
  grows 32→48 bytes for the two slab fields.
- **P1-I-D** ([[chg-2026-05-05-p1id-closing-audit]]) — the closing
  audit ([[adt-p1id-r1]]): reservation disjointness, interior-pointer
  validation, explicit full-slab tracking, the order-corruption guard,
  the struct-size static_assert.

## What aged well and what didn't

The mechanism survived four eras nearly untouched — the later work was
all SMP honesty (#807 magazines, RW-1 locks/guards) layered on this
shape. The DOCS did not survive: both reference docs froze at their
landing chunk and taught pre-audit behavior for fourteen months
([[sub-kernel-mm-phys]] and [[sub-kernel-mm-slub]] carry the
catalogs).
