// kernel-internal exec — load an ELF into a Proc's address space (P3-Eb).
//
// Per ARCHITECTURE.md §16 (process address space). Bridges P2-Ga's
// elf_load (parse + validate) to P3-D's VMA + BURROW machinery: each
// PT_LOAD segment becomes a fresh anonymous BURROW (eager-allocated
// backing pages) populated with blob bytes via the kernel direct map,
// then mapped into the per-Proc TTBR0 tree via burrow_map.
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
#include <thylacine/burrow.h>

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
//   1. Round vaddr range up to page boundaries → size for the BURROW.
//   2. burrow_create_anon(size).
//   3. Copy `filesz` bytes from blob[file_offset..] into the BURROW via
//      direct map. The remaining (memsz - filesz) bytes are already
//      zero (KP_ZERO from alloc_pages).
//   4. burrow_map(p, burrow, vaddr_start, size, prot).
//   5. burrow_unref(burrow) — drop the caller-held handle. mapping_count
//      (held by the VMA) keeps the BURROW alive until proc_free's
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

    // memsz must fit within BURROW addressable range (overflow guard).
    u64 vaddr_end = seg->vaddr + seg->memsz;
    if (vaddr_end < seg->vaddr)             return -1;
    u64 vaddr_end_aligned = round_up_page(vaddr_end);
    if (vaddr_end_aligned == 0)             return -1;
    size_t size = (size_t)(vaddr_end_aligned - seg->vaddr);

    struct Burrow *burrow = burrow_create_anon(size);
    if (!burrow)                               return -1;

    // Copy filesz bytes from blob[file_offset..] → burrow's first page (offset 0).
    // The BURROW's pages start at offset 0; vmaddr_start corresponds to
    // BURROW offset 0 (the segment is page-aligned).
    if (seg->filesz > 0) {
        u8 *burrow_kva = (u8 *)pa_to_kva(page_to_pa(burrow->pages));
        const u8 *src = (const u8 *)blob + seg->file_offset;
        for (size_t i = 0; i < seg->filesz; i++) {
            burrow_kva[i] = src[i];
        }

        // R7 F134 close: cache maintenance for executable segments.
        // Bytes were written via the data path (D-cache); EL0 fetches
        // them via I-cache. ARM ARM B2.4.2 requires `dc cvau` (clean
        // to PoU) + `ic ivau` (invalidate I-cache to PoU) per cache
        // line + DSB/ISB to make instruction visibility atomic with
        // the data write. Without this, the I-cache may serve stale
        // bytes from a prior occupant of the same physical page —
        // critical at Phase 4+ when different binaries can share PA
        // recycling. v1.0 P3-F masks the hazard because /init's blob
        // bytes don't change across iterations.
        //
        // Only required when segment is executable. RW-only segments
        // (data, bss) don't need I-cache maintenance.
        if (seg->flags & PF_X) {
            // ARM ARM cache-line size from CTR_EL0.DminLine; v1.0 uses
            // a conservative 64-byte fixed line (matches Cortex-A72/76
            // DminLine 4 → 16 words = 64B). Phase 5+ may read CTR_EL0
            // dynamically.
            const size_t line = 64;
            uintptr_t start = (uintptr_t)burrow_kva;
            uintptr_t end   = start + seg->filesz;
            uintptr_t addr  = start & ~(uintptr_t)(line - 1);
            for (; addr < end; addr += line) {
                __asm__ __volatile__("dc cvau, %0" :: "r" (addr) : "memory");
            }
            __asm__ __volatile__("dsb ish" ::: "memory");
            addr = start & ~(uintptr_t)(line - 1);
            for (; addr < end; addr += line) {
                __asm__ __volatile__("ic ivau, %0" :: "r" (addr) : "memory");
            }
            __asm__ __volatile__("dsb ish\nisb\n" ::: "memory");
        }
    }
    // [filesz, size) stays zero from KP_ZERO.

    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = burrow_map(p, burrow, seg->vaddr, size, prot);
    if (rc != 0) {
        burrow_unref(burrow);
        return -1;
    }

    // Drop the caller-held handle. mapping_count=1 (from vma_alloc)
    // keeps the BURROW alive; freed at proc_free's vma_drain when
    // mapping_count drops to 0 (and handle_count is 0).
    burrow_unref(burrow);
    return 0;
}

// Map the user stack — a 16 KiB anonymous VMA at the fixed top-of-user-VA.
static int exec_map_user_stack(struct Proc *p) {
    struct Burrow *burrow = burrow_create_anon(EXEC_USER_STACK_SIZE);
    if (!burrow)                               return -1;

    int rc = burrow_map(p, burrow, EXEC_USER_STACK_BASE, EXEC_USER_STACK_SIZE,
                     VMA_PROT_RW);
    if (rc != 0) {
        burrow_unref(burrow);
        return -1;
    }
    burrow_unref(burrow);
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
