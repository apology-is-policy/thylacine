// kernel-internal exec — load an ELF into a Proc's address space (P3-Eb).
//
// Per ARCHITECTURE.md §16 (process address space). Bridges P2-Ga's
// elf_load (parse + validate) to P3-D's VMA + VMO machinery: each
// PT_LOAD segment becomes a fresh anonymous VMO (eager-allocated
// backing pages) populated with blob bytes via the kernel direct map,
// then mapped into the per-Proc TTBR0 tree via vmo_map.
//
// The user stack is a dedicated 16 KiB VMA at `[EXEC_USER_STACK_BASE,
// EXEC_USER_STACK_TOP)`. v1.0 doesn't grow the stack; Phase 5+ adds
// demand-grow on faults below stack base.
//
// At v1.0 P3-Eb the function does NOT transition to EL0 — it only
// populates the Proc's address space + reports the entry + sp top.
// Caller (the future asm trampoline at P3-Ed) configures ERET state
// and eret's to EL0.

#include <thylacine/exec.h>
#include <thylacine/elf.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/vmo.h>

#include "../mm/phys.h"

// =============================================================================
// Helpers
// =============================================================================

// Convert ELF program-header flags (PF_R / PF_W / PF_X) to VMA prot bits.
static u32 vma_prot_for_elf(u32 elf_flags) {
    u32 prot = 0;
    if (elf_flags & PF_R) prot |= VMA_PROT_READ;
    if (elf_flags & PF_W) prot |= VMA_PROT_WRITE;
    if (elf_flags & PF_X) prot |= VMA_PROT_EXEC;
    return prot;
}

// Round x up to the next page boundary. Overflow-safe: returns 0 if the
// rounded value would wrap (caller treats as "too big").
static u64 round_up_page(u64 x) {
    if (x > UINT64_MAX - (PAGE_SIZE - 1)) return 0;
    return (x + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
}

// Map a single PT_LOAD segment into the Proc's address space.
//
// Steps:
//   1. Round vaddr range up to page boundaries → size for the VMO.
//   2. vmo_create_anon(size).
//   3. Copy `filesz` bytes from blob[file_offset..] into the VMO via
//      direct map. The remaining (memsz - filesz) bytes are already
//      zero (KP_ZERO from alloc_pages).
//   4. vmo_map(p, vmo, vaddr_start, size, prot).
//   5. vmo_unref(vmo) — drop the caller-held handle. mapping_count
//      (held by the VMA) keeps the VMO alive until proc_free's
//      vma_drain.
//
// Returns 0 on success, -1 on failure (alignment violation, SLUB OOM,
// vma_insert overlap).
static int exec_map_segment(struct Proc *p, const void *blob,
                            const struct elf_load_segment *seg) {
    // v1.0: require page-aligned vaddr + file_offset. Real toolchains
    // (clang, gcc) page-align by default; the leniency for non-aligned
    // segments lands post-v1.0.
    if (seg->vaddr & (PAGE_SIZE - 1))       return -1;
    if (seg->file_offset & (PAGE_SIZE - 1)) return -1;

    if (seg->memsz == 0)                    return -1;

    // memsz must fit within VMO addressable range (overflow guard).
    u64 vaddr_end = seg->vaddr + seg->memsz;
    if (vaddr_end < seg->vaddr)             return -1;
    u64 vaddr_end_aligned = round_up_page(vaddr_end);
    if (vaddr_end_aligned == 0)             return -1;
    size_t size = (size_t)(vaddr_end_aligned - seg->vaddr);

    struct Vmo *vmo = vmo_create_anon(size);
    if (!vmo)                               return -1;

    // Copy filesz bytes from blob[file_offset..] → vmo's first page (offset 0).
    // The VMO's pages start at offset 0; vmaddr_start corresponds to
    // VMO offset 0 (the segment is page-aligned).
    if (seg->filesz > 0) {
        u8 *vmo_kva = (u8 *)pa_to_kva(page_to_pa(vmo->pages));
        const u8 *src = (const u8 *)blob + seg->file_offset;
        for (size_t i = 0; i < seg->filesz; i++) {
            vmo_kva[i] = src[i];
        }
    }
    // [filesz, size) stays zero from KP_ZERO.

    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = vmo_map(p, vmo, seg->vaddr, size, prot);
    if (rc != 0) {
        vmo_unref(vmo);
        return -1;
    }

    // Drop the caller-held handle. mapping_count=1 (from vma_alloc)
    // keeps the VMO alive; freed at proc_free's vma_drain when
    // mapping_count drops to 0 (and handle_count is 0).
    vmo_unref(vmo);
    return 0;
}

// Map the user stack — a 16 KiB anonymous VMA at the fixed top-of-user-VA.
static int exec_map_user_stack(struct Proc *p) {
    struct Vmo *vmo = vmo_create_anon(EXEC_USER_STACK_SIZE);
    if (!vmo)                               return -1;

    int rc = vmo_map(p, vmo, EXEC_USER_STACK_BASE, EXEC_USER_STACK_SIZE,
                     VMA_PROT_RW);
    if (rc != 0) {
        vmo_unref(vmo);
        return -1;
    }
    vmo_unref(vmo);
    return 0;
}

// =============================================================================
// Public entry.
// =============================================================================

int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out) {
    if (!p || !blob || !entry_out || !sp_out) return -1;
    if (p->magic != PROC_MAGIC)                return -1;
    if (p->pgtable_root == 0)                  return -1;     // kproc rejected
    // v1.0: no replace-in-place; p must be clean.
    if (p->vmas != NULL)                       return -1;

    struct elf_image img;
    int r = elf_load(blob, blob_size, &img);
    if (r != ELF_LOAD_OK)                      return -1;

    // Map each PT_LOAD segment.
    for (int i = 0; i < img.n_segments; i++) {
        if (exec_map_segment(p, blob, &img.segments[i]) != 0) {
            // Partial state: caller disposes the Proc.
            return -1;
        }
    }

    // Allocate user stack.
    if (exec_map_user_stack(p) != 0) {
        return -1;
    }

    *entry_out = img.entry;
    *sp_out    = EXEC_USER_STACK_TOP;
    return 0;
}
