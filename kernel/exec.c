// kernel-internal exec — load an ELF into a Proc's address space (P3-Eb).
//
// Per ARCHITECTURE.md §16 (process address space). Bridges P2-Ga's
// elf_load (parse + validate) to P3-D's VMA + BURROW machinery: each
// PT_LOAD segment becomes a fresh anonymous BURROW (eager-allocated
// backing pages) populated with blob bytes via the kernel direct map,
// then mapped into the per-Proc TTBR0 tree via burrow_map.
//
// The user stack is a dedicated 256 KiB VMA at `[EXEC_USER_STACK_BASE,
// EXEC_USER_STACK_TOP)` (sized for ML-KEM-768's stack-heavy FO transform
// in corvus — see exec.h). v1.0 doesn't grow the stack; Phase 5+ adds
// demand-grow on faults below stack base. The top EXEC_INIT_STACK_SIZE
// bytes carry the System V process-startup frame (argc / argv / envp /
// auxv) a C runtime reads at entry — see exec.h + exec_build_init_stack.
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
#include <thylacine/random.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>
#include <thylacine/dev.h>      // REVENANT R-4: exe->dev->read for the file-backed path
#include <thylacine/spoor.h>    // REVENANT R-4: spoor_ref / spoor_clunk
#include <thylacine/image.h>    // REVENANT R-4: image_lookup_or_create (shared text)

#include "../mm/phys.h"
#include "../mm/slub.h"         // REVENANT R-4: kmalloc/kfree for the ELF header read

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
        // (data, bss) don't need I-cache maintenance. REVENANT R-4 routed
        // this through the shared arch helper (correct CTR_EL0 line sizes
        // vs the prior hardcoded 64B; same helper the FILE fault arm uses).
        if (seg->flags & PF_X)
            arch_icache_sync_range(burrow_kva, seg->filesz);
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

// Map the user stack — a 256 KiB anonymous VMA at the fixed top-of-
// user-VA — plus a one-page guard VMA directly below it.
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

    // P5-secondary-stack-guard: install a one-page guard VMA directly
    // below the stack. It is a reserved, prot==0, no-BURROW range — an
    // overflow past EXEC_USER_STACK_BASE faults (userland_demand_page
    // rejects prot==0) instead of corrupting a lower VMA, and
    // vma_insert's overlap rejection keeps a future mapping out of the
    // page. On vma_insert failure (an ELF segment already occupies the
    // range — correctly rejected) the guard is freed and exec_setup
    // disposes the partially-built Proc.
    struct Vma *guard = vma_alloc_guard(EXEC_USER_STACK_GUARD_BASE,
                                        EXEC_USER_STACK_BASE);
    if (!guard)                                return -1;
    if (vma_insert(p, guard) != 0) {
        vma_free(guard);
        return -1;
    }
    return 0;
}

// Build the System V initial-process stack frame — argc / argv / envp /
// auxv (+ argv strings region under Shape B) — at the top of the user
// stack. See exec.h for the byte layout.
//
// Two shapes (selected by whether argv_data is non-NULL and argc > 0):
//   Shape A — no argv (legacy: argc == 0; fixed-size frame =
//             EXEC_INIT_STACK_SIZE = 144 bytes).
//   Shape B — argv-bearing (P6-pouch-stratumd-boot sub-chunk 16b-alpha;
//             variable-size frame bounded by EXEC_INIT_STACK_MAX_SIZE).
//
// The stack BURROW (installed by exec_map_user_stack) is located via
// vma_lookup and written through the kernel direct map; offset 0 of the
// BURROW corresponds to EXEC_USER_STACK_BASE, so the frame's bytes land
// in the BURROW's last `frame_size` bytes. Returns the initial user sp —
// the user VA of the frame's `argc` word.
_Static_assert(EXEC_INIT_AUXV_COUNT == 6,
               "exec_build_init_stack writes exactly 6 auxv entries by "
               "literal index — adding an entry means editing the auxv "
               "block below, not just bumping the exec.h macro");
static u64 exec_build_init_stack(struct Proc *p, const struct elf_image *img,
                                 const char *argv_data, u32 argv_data_len,
                                 u32 argc) {
    // Shape selection. argv_data_len > 0 iff argc > 0 (caller's
    // invariant enforced at the syscall body); we re-check the
    // structural invariant here as defense-in-depth.
    bool has_argv = (argc > 0);
    if (has_argv) {
        if (argv_data_len == 0 || !argv_data)
            extinction("exec_build_init_stack: argc > 0 with no argv_data");
        if (argc > 512u)                   // mirrors SYS_SPAWN_ARGV_MAX — kept
                                           // local to this file to avoid a
                                           // syscall.h include cycle
            extinction("exec_build_init_stack: argc exceeds bound");
        if (argv_data_len > 65536u)        // mirrors SYS_SPAWN_ARGV_DATA_MAX
            extinction("exec_build_init_stack: argv_data_len exceeds bound");
        if (argv_data[argv_data_len - 1] != '\0')
            extinction("exec_build_init_stack: argv_data not NUL-terminated");
    }

    // exec_map_user_stack returned 0 → the stack VMA + its BURROW exist.
    struct Vma *sv = vma_lookup(p, EXEC_USER_STACK_BASE);
    if (!sv || !sv->burrow)
        extinction("exec_build_init_stack: stack VMA missing");
    u8 *stack_kva = (u8 *)pa_to_kva(page_to_pa(sv->burrow->pages));

    // Compute frame layout.
    //
    // Shape A: fixed-size 144-byte frame; same as the legacy layout.
    // Shape B:
    //   structured_top_bytes = 8 (argc) + 8*(argc+1) (argv[]+NULL)
    //                          + 8 (envp NULL) + 96 (auxv) = 120 + 8*argc.
    //   strings_region_offset = round_up(structured_top_bytes, 16) + 16
    //                            (the 16-aligned AT_RANDOM block precedes
    //                            the strings region).
    //   frame_size            = round_up(strings_region_offset + argv_data_len, 16).
    u64 frame_size;
    u64 random_offset_from_sp;  // offset of the 16-byte AT_RANDOM block

    if (!has_argv) {
        frame_size            = EXEC_INIT_STACK_SIZE;
        random_offset_from_sp = EXEC_INIT_RANDOM_OFFSET;
    } else {
        u64 structured = 8 + (u64)(argc + 1u) * 8u + 8u + (u64)EXEC_INIT_AUXV_COUNT * 16u;
        random_offset_from_sp = (structured + 15u) & ~15ull;
        u64 strings_offset    = random_offset_from_sp + 16u;
        u64 unrounded         = strings_offset + (u64)argv_data_len;
        frame_size            = (unrounded + 15u) & ~15ull;
    }

    u8 *frame = stack_kva + EXEC_USER_STACK_SIZE - frame_size;
    u64 sp    = EXEC_USER_STACK_TOP - frame_size;

    // AT_PHDR — the user VA of the program-header table. The phdrs sit
    // at file offset img->phoff; find the PT_LOAD segment whose file
    // range covers the whole table and translate offset → user VA. A
    // well-formed static binary's first PT_LOAD spans the ELF header +
    // phdrs. If none covers them, the entire phdr triple is reported 0.
    u64 phdr_va = 0;
    u64 phnum   = 0;
    u64 phent   = 0;
    {
        u64 phtab_bytes = (u64)img->phnum * (u64)img->phentsize;
        for (int i = 0; i < img->n_segments; i++) {
            const struct elf_load_segment *s = &img->segments[i];
            if (img->phoff >= s->file_offset &&
                (img->phoff - s->file_offset) + phtab_bytes <= s->filesz) {
                phdr_va = s->vaddr + (img->phoff - s->file_offset);
                phnum   = img->phnum;
                phent   = img->phentsize;
                break;
            }
        }
    }

    // AT_RANDOM — 16 bytes of kernel-CSPRNG entropy. Same shape across
    // both layouts; the in-frame offset differs.
    u8 rand[16] = {0};
    (void)kern_random_bytes(rand, sizeof(rand));

    // Lay out the frame. The frame base is 16-byte aligned (exec.h
    // _Static_assert + the round_up above); every u64 below is 8-aligned.
    u64 *w = (u64 *)frame;
    if (!has_argv) {
        // Shape A — argc=0, single NULL terminator for argv[].
        w[0]  = 0;                           // argc
        w[1]  = 0;                           // argv[0] — NULL terminator
        w[2]  = 0;                           // envp[0] — NULL terminator
        w[3]  = AT_PHDR;    w[4]  = phdr_va;
        w[5]  = AT_PHENT;   w[6]  = phent;
        w[7]  = AT_PHNUM;   w[8]  = phnum;
        w[9]  = AT_PAGESZ;  w[10] = PAGE_SIZE;
        w[11] = AT_RANDOM;  w[12] = sp + EXEC_INIT_RANDOM_OFFSET;
        w[13] = AT_NULL;    w[14] = 0;
        // w[15] is the 8-byte alignment pad — left zero.
    } else {
        // Shape B — argc real, argv[] points into the strings region.
        // The strings region starts at (sp + random_offset_from_sp + 16);
        // walk argv_data to compute each argv[i]'s user-VA.
        u64 strings_user_va = sp + random_offset_from_sp + 16u;

        w[0] = (u64)argc;                    // argc

        // argv[0..argc-1] point into the strings region. Walk argv_data
        // NUL-by-NUL to find each string's start offset; argv[0] starts
        // at offset 0, argv[i] at the byte AFTER the (i-1)-th NUL.
        u32 cur_arg = 0;
        u32 next_start = 0;
        w[1 + 0] = strings_user_va + 0;       // argv[0] starts at offset 0
        for (u32 i = 0; i < argv_data_len; i++) {
            if (argv_data[i] == '\0') {
                cur_arg++;
                next_start = i + 1;
                if (cur_arg < argc) {
                    w[1 + cur_arg] = strings_user_va + next_start;
                }
            }
        }
        if (cur_arg != argc)
            extinction("exec_build_init_stack: NUL count != argc post-validation");

        w[1 + argc]   = 0;                   // argv[argc] = NULL terminator
        w[2 + argc]   = 0;                   // envp[0]    = NULL terminator

        // auxv at w[3 + argc .. 14 + argc]; 12 u64s (6 entries × 2).
        u64 *auxv = &w[3 + argc];
        auxv[0]  = AT_PHDR;   auxv[1]  = phdr_va;
        auxv[2]  = AT_PHENT;  auxv[3]  = phent;
        auxv[4]  = AT_PHNUM;  auxv[5]  = phnum;
        auxv[6]  = AT_PAGESZ; auxv[7]  = PAGE_SIZE;
        auxv[8]  = AT_RANDOM; auxv[9]  = sp + random_offset_from_sp;
        auxv[10] = AT_NULL;   auxv[11] = 0;

        // Copy argv strings into the strings region.
        u8 *strings_dst = frame + random_offset_from_sp + 16u;
        for (u32 i = 0; i < argv_data_len; i++) strings_dst[i] = (u8)argv_data[i];
    }

    u8 *rand_dst = frame + random_offset_from_sp;
    for (size_t i = 0; i < sizeof(rand); i++) rand_dst[i] = rand[i];

    return sp;
}

// =============================================================================
// Public entry.
// =============================================================================

// Internal: the unified exec body. exec_setup + exec_setup_with_argv are
// thin wrappers that pass NULL/0/0 or the caller's argv buffer
// respectively. Sharing the body avoids the divergence the earlier copy-
// paste pattern would accumulate; argv flows transparently through to
// exec_build_init_stack.
static int exec_setup_argv_body(struct Proc *p,
                                const void *blob, size_t blob_size,
                                const char *argv_data, u32 argv_data_len,
                                u32 argc,
                                u64 *entry_out, u64 *sp_out) {
    if (!p || !blob || !entry_out || !sp_out) return -1;
    if (p->magic != PROC_MAGIC)                return -1;
    if (p->pgtable_root == 0)                  return -1;     // kproc rejected
    // v1.0: no replace-in-place; p must be clean.
    if (p->vmas != NULL)                       return -1;

    // argv invariants (defense-in-depth — the syscall body has already
    // checked these, but exec_setup_with_argv is exported to other
    // callers and the contract is owned here too).
    if (argc > 0) {
        if (argv_data_len == 0 || !argv_data)  return -1;
        if (argv_data[argv_data_len - 1] != '\0') return -1;
    } else {
        if (argv_data_len != 0)                return -1;
    }

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

    // Build the System V startup frame (argc / argv / envp / auxv +
    // strings region under Shape B) at the top of the user stack;
    // *sp_out points at its `argc` word.
    *entry_out = img.entry;
    *sp_out    = exec_build_init_stack(p, &img, argv_data, argv_data_len, argc);
    return 0;
}

int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out) {
    return exec_setup_argv_body(p, blob, blob_size,
                                /*argv_data=*/NULL, /*argv_data_len=*/0u,
                                /*argc=*/0u,
                                entry_out, sp_out);
}

int exec_setup_with_argv(struct Proc *p, const void *blob, size_t blob_size,
                         const char *argv_data, u32 argv_data_len, u32 argc,
                         u64 *entry_out, u64 *sp_out) {
    return exec_setup_argv_body(p, blob, blob_size,
                                argv_data, argv_data_len, argc,
                                entry_out, sp_out);
}

// =============================================================================
// REVENANT R-4: the file-backed production exec path.
// =============================================================================
//
// docs/REVENANT.md §4.2. exec_setup_from_spoor reads ONLY the ELF header+phdrs
// from the pinned executable, then maps each PT_LOAD: executable text (whose
// memsz fits within file-backed pages) is SHARED file-backed via the Image
// cache (demand-paged by the R-2 fault arm); everything else is a private
// eager-copied anonymous segment. This retires the whole-binary slurp -- a
// binary of any size execs.

// map_text_file_backed -- a PF_X PT_LOAD as SHARED file-backed text. Gated by
// the caller to round_up(filesz) == round_up(memsz) (every mapped page has a
// file page behind it; the last partial page's tail is the file's zero padding
// between page-aligned segments, or EOF -> the fault arm's KP_ZERO). BORROWS
// exe; spoor_refs a fresh ref for the consuming Image lookup.
static int map_text_file_backed(struct Proc *p, struct Spoor *exe,
                                const struct elf_load_segment *seg) {
    u64 vaddr_end = seg->vaddr + seg->memsz;
    if (vaddr_end < seg->vaddr)             return -1;
    u64 vaddr_end_aligned = round_up_page(vaddr_end);
    if (vaddr_end_aligned == 0)             return -1;
    size_t size = (size_t)(vaddr_end_aligned - seg->vaddr);

    // image_lookup_or_create CONSUMES one spoor ref (adopts on miss / clunks on
    // hit). The thunk keeps the borrowed `exe`, so hand the lookup a FRESH ref;
    // on a NULL return it consumed nothing -> drop the fresh ref.
    spoor_ref(exe);
    struct Burrow *b = image_lookup_or_create(exe, seg->file_offset, seg->filesz);
    if (!b) { spoor_clunk(exe); return -1; }

    // R+X. W^X (I-12) holds by construction: elf_load rejected PF_W|PF_X, so a
    // PF_X segment is never writable.
    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = burrow_map(p, b, seg->vaddr, size, prot);
    // Drop the caller's handle ref. On success mapping_count + the cache's
    // strong ref keep the image alive; on map failure the image stays validly
    // cached (idle) for a later exec -- never leaked.
    burrow_unref(b);
    return rc == 0 ? 0 : -1;
}

// map_eager_from_file -- a non-shared PT_LOAD eager-copied from the file into a
// PRIVATE anonymous Burrow: data (R+W), rodata (R), or a rare PF_X segment whose
// memsz extends past the file's last page (whole bss pages the file-backed path
// cannot zero). filesz bytes are dev->read; the [filesz, size) tail stays zero
// (KP_ZERO) = .bss. No userspace file-backed writable mapping is ever created.
static int map_eager_from_file(struct Proc *p, struct Spoor *exe,
                               const struct elf_load_segment *seg) {
    u64 vaddr_end = seg->vaddr + seg->memsz;
    if (vaddr_end < seg->vaddr)             return -1;
    u64 vaddr_end_aligned = round_up_page(vaddr_end);
    if (vaddr_end_aligned == 0)             return -1;
    size_t size = (size_t)(vaddr_end_aligned - seg->vaddr);

    struct Burrow *b = burrow_create_anon(size);
    if (!b)                                  return -1;

    if (seg->filesz > 0) {
        u8 *kva = (u8 *)pa_to_kva(page_to_pa(b->pages));
        size_t got = 0;
        while (got < seg->filesz) {
            long n = exe->dev->read(exe, kva + got, (long)(seg->filesz - got),
                                    (s64)(seg->file_offset + got));
            if (n < 0)  { burrow_unref(b); return -1; }   // #811-interruptible / I/O error
            if (n == 0) break;                             // short read (caught below)
            got += (size_t)n;
        }
        if (got != seg->filesz) { burrow_unref(b); return -1; }   // truncated -> no partial map

        // A PF_X segment routed here (memsz extends past the file pages) still
        // executes its loaded bytes -> I-cache maintenance on the copied range.
        if (seg->flags & PF_X)
            arch_icache_sync_range(kva, seg->filesz);
    }
    // [filesz, size) stays zero (KP_ZERO from alloc_pages) = .bss.

    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = burrow_map(p, b, seg->vaddr, size, prot);
    burrow_unref(b);
    return rc == 0 ? 0 : -1;
}

// Read the ELF header + program-header table from the pinned executable into a
// kmalloc'd buffer, bounded so elf_load (which derefs the ehdr + ph[]) never
// reads past it. Returns the buffer (caller kfree) + *got_out, or NULL on any
// failure. elf_load re-validates everything against the real file size; this is
// purely the OOB-deref guard for handing it a header-only buffer.
static void *exec_read_header(struct Spoor *exe, size_t *got_out) {
    *got_out = 0;
    void *hdr = kmalloc(EXEC_ELF_HEADER_MAX, 0);
    if (!hdr)                                  return NULL;
    if (((uintptr_t)hdr & 0x7) != 0)           { kfree(hdr); return NULL; }  // 8-align the Ehdr cast

    u8 *dst = (u8 *)hdr;
    size_t got = 0;
    while (got < EXEC_ELF_HEADER_MAX) {
        long n = exe->dev->read(exe, dst + got, (long)(EXEC_ELF_HEADER_MAX - got),
                                (s64)got);
        if (n < 0) { kfree(hdr); return NULL; }   // #811-interruptible / I/O error
        if (n == 0) break;                        // EOF (a header smaller than the cap)
        got += (size_t)n;
    }

    // Bound the phdr table within what we read, so elf_load's ph[0..phnum) deref
    // stays in-bounds. elf_load enforces phentsize == sizeof(Phdr) before it
    // indexes ph[] (returning early otherwise) and uses that fixed stride; the
    // span below mirrors it. Overflow-safe: got <= EXEC_ELF_HEADER_MAX (16 KiB)
    // and phnum is a u16, so span <= 65535*56 fits a u64 with no wrap, and the
    // phoff > got check runs before the subtraction.
    if (got < sizeof(struct Elf64_Ehdr))       { kfree(hdr); return NULL; }
    const struct Elf64_Ehdr *eh = (const struct Elf64_Ehdr *)hdr;
    if (eh->e_phentsize != sizeof(struct Elf64_Phdr)) { kfree(hdr); return NULL; }
    u64 phoff = eh->e_phoff;
    u64 span  = (u64)eh->e_phnum * sizeof(struct Elf64_Phdr);
    if (phoff > (u64)got || (u64)got - phoff < span)  { kfree(hdr); return NULL; }

    *got_out = got;
    return hdr;
}

int exec_setup_from_spoor(struct Proc *p, struct Spoor *exe, size_t exe_size,
                          const char *argv_data, u32 argv_data_len, u32 argc,
                          u64 *entry_out, u64 *sp_out) {
    if (!p || !exe || !entry_out || !sp_out)   return -1;
    if (p->magic != PROC_MAGIC)                return -1;
    if (p->pgtable_root == 0)                  return -1;     // kproc rejected
    if (p->vmas != NULL)                       return -1;     // clean address space only
    if (!exe->dev || !exe->dev->read)          return -1;
    if (exe_size == 0 || exe_size > EXEC_FILE_MAX) return -1;

    // argv invariants (defense-in-depth; the syscall body already checked them,
    // but the contract is owned here too -- exec_setup_from_spoor is exported).
    if (argc > 0) {
        if (argv_data_len == 0 || !argv_data)      return -1;
        if (argv_data[argv_data_len - 1] != '\0')  return -1;
    } else {
        if (argv_data_len != 0)                    return -1;
    }

    // 1. Read + parse ONLY the ELF header + phdrs (a few KB), not the whole
    //    binary. elf_load validates segment extents against the real file size.
    size_t hdr_got = 0;
    void *hdr = exec_read_header(exe, &hdr_got);
    if (!hdr)                                  return -1;
    struct elf_image img;
    int r = elf_load(hdr, exe_size, &img);
    kfree(hdr);
    if (r != ELF_LOAD_OK)                      return -1;

    // 2. Map each PT_LOAD. Executable text whose memsz fits within file-backed
    //    pages is SHARED file-backed (demand-paged); everything else is a
    //    private eager-copied anon segment. Page-aligned vaddr + file_offset is
    //    required (matches exec_map_segment): the fault arm + the eager copy
    //    both assume burrow-offset-0 == seg->vaddr.
    for (int i = 0; i < img.n_segments; i++) {
        const struct elf_load_segment *seg = &img.segments[i];
        if (seg->vaddr & (PAGE_SIZE - 1))       return -1;
        if (seg->file_offset & (PAGE_SIZE - 1)) return -1;
        if (seg->memsz == 0)                    return -1;

        // text_shareable iff PF_X AND round_up(filesz) == round_up(memsz) (no
        // whole bss page beyond the file). vaddr is page-aligned, so the
        // vaddr-relative rounded ends compare the rounded sizes; a 0 from
        // round_up_page is overflow -> fall to the eager path (which re-rejects).
        u64 fend = round_up_page(seg->vaddr + seg->filesz);
        u64 mend = round_up_page(seg->vaddr + seg->memsz);
        bool text_shareable = (seg->flags & PF_X) && fend != 0 && fend == mend;

        int rc = text_shareable ? map_text_file_backed(p, exe, seg)
                                : map_eager_from_file(p, exe, seg);
        if (rc != 0)                            return -1;   // partial -> caller disposes Proc
    }

    // 3. User stack + the System V startup frame (unchanged; reads img metadata,
    //    not the file -- AT_PHDR resolves into the first mapped segment's VA).
    if (exec_map_user_stack(p) != 0)           return -1;
    *entry_out = img.entry;
    *sp_out    = exec_build_init_stack(p, &img, argv_data, argv_data_len, argc);
    return 0;
}
