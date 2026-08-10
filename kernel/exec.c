// kernel-internal exec — load an ELF into a Proc's address space (P3-Eb).
//
// Per ARCHITECTURE.md §16 (process address space). Bridges P2-Ga's
// elf_load (parse + validate) to P3-D's VMA + BURROW machinery: each
// PT_LOAD segment becomes a fresh anonymous BURROW (eager-allocated
// backing pages) populated with blob bytes via the kernel direct map,
// then mapped into the per-Proc TTBR0 tree via burrow_map.
//
// The user stack is a dedicated 1 MiB VMA at `[EXEC_USER_STACK_BASE,
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
#include <thylacine/env.h>      // #140: env_render_environ (the /env -> envp projection)
#include <thylacine/errno.h>    // #140: T_E_2BIG / T_E_NOMEM from exec_stage_env
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/random.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>
#include <thylacine/vdso.h>     // vdso_clock_burrow (map the RO clock page, #343)
#include "../arch/arm64/hwfeat.h" // g_hw_features.linux_hwcap (the AT_HWCAP word)
#include <thylacine/dev.h>      // REVENANT R-4: exe->dev->read for the file-backed path
#include <thylacine/spoor.h>    // REVENANT R-4: spoor_ref / spoor_clunk
#include <thylacine/path.h>     // prowl-1: exe->path->s -- the process name source
#include <thylacine/image.h>    // REVENANT R-4: image_lookup_or_create (shared text)

#include "../arch/arm64/uart.h" // VIVARIUM 12.1 rule 4: the brand-mismatch diagnostic
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

// #107 test observable: the span the eager paths last asked the arch layer to
// make instruction-coherent, plus a monotonic count of those requests. Kept
// HERE rather than in arch_icache_sync_range so only the exec path can write it
// -- a concurrent demand-page sync on another CPU cannot pollute what the test
// reads (and the arch helper, which runs per page-in fault, gains nothing).
//
// The COUNT is not decoration (#107-audit F3). The invoked precedent --
// W1.5's `g_alt_applied == g_alt_total` -- is a COMPLETENESS equality, and a
// last-call snapshot is not: an ELF with two PF_X PT_LOADs calls the recorder
// twice and the scalars hold only the second, so a sabotage that skips the
// sync for every segment but the last is invisible to a span-only assertion.
// N syncs for N PF_X segments is the posture actually realized.
//
// KERNEL_TESTS-gated (#107-audit F4): the production shape (KERNEL_TESTS=OFF)
// carries neither the storage nor the stores, matching burrow.c / image.c.
//
// #130-R2 F4 corrects what that gating claims. It removes the stores from
// PRODUCTION, which is worth doing on its own; it does NOT retire the
// concurrency question, because the shape that reads them is exactly the shape
// that has them. In a KERNEL_TESTS build these remain unsynchronized cross-CPU
// writes whose safety rests on a property of the SUITE ("no other exec runs
// concurrently"), never of the code -- and the COUNT is read as a DELTA across
// a call, so it is strictly more fragile than the scalars it supplements: a
// concurrent exec perturbs a difference even where it would leave a last-write
// snapshot plausible. The suite is UP-driven at this point, so the property
// holds today; it is an assumption to re-check, not a question that was closed.
#ifdef KERNEL_TESTS
static u64    g_exec_icache_last_addr;
static size_t g_exec_icache_last_len;
static u64    g_exec_icache_calls;

void exec_icache_last_for_test(u64 *addr_out, size_t *len_out) {
    if (addr_out) *addr_out = g_exec_icache_last_addr;
    if (len_out)  *len_out  = g_exec_icache_last_len;
}

u64 exec_icache_calls_for_test(void) { return g_exec_icache_calls; }

#define EXEC_ICACHE_RECORD(kva, len)                          \
    do {                                                      \
        g_exec_icache_last_addr = (u64)(uintptr_t)(kva);      \
        g_exec_icache_last_len  = (len);                      \
        g_exec_icache_calls++;                                \
    } while (0)
#else
#define EXEC_ICACHE_RECORD(kva, len) do { (void)(kva); (void)(len); } while (0)
#endif

// Make an eagerly-populated executable segment instruction-coherent.
//
// `len` is the whole PAGE-ROUNDED segment span, NOT the copied byte count
// (#107). The bytes past filesz are zeroed by KP_ZERO, but that zeroing is a
// DATA-side write: it does not evict I-cache lines that a prior occupant of
// these recycled PAs may have left behind, and the tail is mapped executable
// along with the rest of the segment. A branch into it would then fetch stale
// instructions rather than trapping on the zeros. This is the rule the FILE
// fault arm already applies (arch/arm64/fault.c syncs PAGE_SIZE, not the valid
// byte count) -- the eager paths are the two that had drifted from it.
static void exec_make_exec_coherent(void *kva, size_t len) {
    EXEC_ICACHE_RECORD(kva, len);
    arch_icache_sync_range(kva, len);
}

// LINEAGE L-4a: the number of whole pages spanning `bytes` (0 for 0 bytes).
static size_t pages_spanning(u64 bytes) {
    if (bytes == 0) return 0;
    return (size_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

// L-7 F4: burrow_lazy_populate charges the whole run to `as` on SUCCESS, so every
// failure after it owes the charge back. The pages themselves go with the Burrow's
// last unref; only the counter needs unwinding, and pairing the two here keeps them
// together rather than asking four call sites to remember -- the #106/#130 lesson
// on the very axis that arc established. `exempt` is deliberately not a parameter:
// addrspace_charge_pages COUNTS regardless of exemption (exemption bypasses the
// CAP, not the accounting), so the uncharge must be unconditional to stay paired.
// Give the charge back. Call this ONLY where the populate SUCCEEDED: its own
// failure arm already unwinds, so uncharging there too would under-count.
static void lazy_populate_uncharge(struct AddrSpace *as, size_t nfile) {
    if (nfile == 0) return;
    spin_lock(&as->lock);                // the stated precondition of the counter ops
    addrspace_uncharge_pages(as, (u32)nfile);
    spin_unlock(&as->lock);
}

// The common shape: give the charge back AND drop the Burrow. Returns -1 so the
// arms can `return lazy_populate_unwind(...)` in one line. The stack-frame site
// uses the bare uncharge instead -- it does not own its Burrow.
static int lazy_populate_unwind(struct AddrSpace *as, struct Burrow *v, size_t nfile) {
    lazy_populate_uncharge(as, nfile);
    burrow_unref(v);
    return -1;
}

// LINEAGE L-4a: copy `len` bytes into a POPULATED ANON_LAZY Burrow at byte offset
// `off`, splitting at page boundaries -- the sparse twin of the single memcpy the
// eager paths do into one contiguous chunk. Every page the range touches must
// already be resident (burrow_lazy_populate); a NULL slot means the caller
// populated the wrong run, so fail loudly rather than silently drop the bytes.
static int lazy_write(struct Burrow *v, u64 off, const void *src, size_t len) {
    const u8 *s = (const u8 *)src;
    while (len > 0) {
        size_t slot    = (size_t)(off / PAGE_SIZE);
        size_t in_page = (size_t)(off % PAGE_SIZE);
        size_t chunk   = (size_t)PAGE_SIZE - in_page;
        if (chunk > len) chunk = len;
        u8 *dst = (u8 *)burrow_lazy_slot_kva(v, slot);
        if (!dst) return -1;
        for (size_t i = 0; i < chunk; i++) dst[in_page + i] = s[i];
        off += chunk;
        s   += chunk;
        len -= chunk;
    }
    return 0;
}

// LINEAGE L-4a: may this segment's private anonymous backing be SPARSE?
//
// Only when it is NOT executable. A demand-zeroed page arrives through the
// ANON_LAZY fault arm, which performs NO I-cache maintenance -- so an executable
// bss tail would map executable with a prior occupant's lines still in the I-cache
// (#107's hazard, which the eager paths close by syncing the whole span and the
// REVENANT file-backed arm closes per page). Rather than teach a third arm to sync,
// keep every executable page on a path that already does.
//
// This costs nothing: W^X (I-12) makes PF_W imply !PF_X, so all WRITABLE data --
// which is the whole of what COW must break, and where the eager bss cost lives
// (#130) -- is covered. A text segment is shared read-only and never breaks.
static bool seg_may_be_sparse(const struct elf_load_segment *seg) {
    return (seg->flags & PF_X) == 0;
}

// #149: report a refusal from the exec path.
//
// Every failure here returns a bare -1 into a spawn thunk that has ALREADY
// returned a pid to the parent, so a refused load is indistinguishable from a
// program that ran and chose to exit 1: no syscall, no fault, nothing in the
// log naming that pid. That gap is what made this file's own PT_LOAD-alignment
// reject cost a session to find at the L-6c gate. The loader knows exactly why
// it refused; this is where it says so. uart_puts (not the console path) to
// match the brand-mismatch diagnostic below and to stay non-blocking -- exec
// runs on a fresh preemptible thread and must not acquire the console role.
static void exec_report_fail(const char *why, u64 detail) {
    uart_puts("exec: ");
    uart_puts(why);
    uart_puts(" ");
    uart_puthex64(detail);      // emits its own "0x" prefix
    uart_puts("\n");
}

// #149: the page-aligned mapping geometry of one PT_LOAD.
//
// ELF does NOT require a page-aligned p_vaddr. It requires only
// `p_vaddr == p_offset (mod p_align)`, and every real linker uses that
// latitude: on aarch64 p_align is 64 KiB, so the data segment lands at whatever
// intra-page offset continues the file mapping (busybox's is 0x51d2e0, +0x2e0;
// 20 of 20 ELFs in a stock Alpine rootfs have one). Everything OUR toolchain
// emits is page-aligned, which is why the old blanket reject held for years and
// then met the first foreign ELF -- #136's shape exactly.
//
// So a segment maps from the page-aligned FLOOR of its vaddr, with its own
// bytes placed `lead` bytes into the Burrow. The leading slack reads as ZERO --
// deliberately, not incidentally:
//   - those bytes lie outside [p_vaddr, p_vaddr+p_memsz), so no segment claims
//     them and nothing may rely on their content;
//   - filling them from the file would map bytes the program never asked for
//     (in a gap between segments that is section headers / symtab / debug
//     info), a gratuitous exposure Linux accepts for mmap fidelity and we have
//     no reason to.
// Both eager arms get the zero for free: KP_ZERO on the eager Burrow, the
// demand-zero fault arm on the lazy one.
struct seg_geom {
    u64    floor;   // page-aligned VA that Burrow offset 0 corresponds to
    size_t lead;    // vaddr - floor: leading slack inside the first page
    size_t size;    // Burrow size: floor .. round_up(vaddr + memsz)
};

// Returns 0 and fills *g, or -1 if the segment is degenerate (memsz 0) or its
// end wraps. Does NOT gate alignment -- that is the point.
static int seg_geometry(const struct elf_load_segment *seg, struct seg_geom *g) {
    if (seg->memsz == 0)                       return -1;
    u64 end = seg->vaddr + seg->memsz;
    if (end < seg->vaddr)                      return -1;   // wrap
    u64 end_aligned = round_up_page(end);
    if (end_aligned == 0)                      return -1;   // wrap
    u64 floor = seg->vaddr & ~(u64)(PAGE_SIZE - 1);
    g->floor  = floor;
    g->lead   = (size_t)(seg->vaddr - floor);
    g->size   = (size_t)(end_aligned - floor);
    return 0;
}

// #149: the pages a segment's FILE bytes occupy within its Burrow, counted from
// Burrow offset 0. The bytes span [lead, lead+filesz), and lead < PAGE_SIZE, so
// the run always starts at slot 0 and is pages_spanning(lead + filesz) long.
static size_t seg_file_pages(const struct seg_geom *g, u64 filesz) {
    if (filesz == 0) return 0;
    return pages_spanning((u64)g->lead + filesz);
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
static int exec_map_segment(struct AddrSpace *as, bool exempt, const void *blob,
                            const struct elf_load_segment *seg) {
    // #149: map from the page-aligned FLOOR of vaddr, carrying the intra-page
    // offset. No alignment gate -- ELF does not require one (see seg_geometry).
    // file_offset needs no gate either on this arm: the bytes are COPIED from
    // an arbitrary blob offset, so nothing here assumes a file-page identity.
    struct seg_geom g;
    if (seg_geometry(seg, &g) != 0) {
        exec_report_fail("degenerate PT_LOAD (memsz 0 or wrapping end) at vaddr",
                         seg->vaddr);
        return -1;
    }

    // LINEAGE L-4a: a non-executable segment's backing is SPARSE -- only the pages
    // the FILE covers are populated here; [filesz, size) is .bss and demand-zeroes
    // on first touch. joey's RW PT_LOAD is 8 bytes of data behind 345 KiB of bss,
    // and corvus's is 128 bytes behind 24 MiB (#130); eagerly allocating AND zeroing
    // the whole memsz at every exec was the cost. Executable segments stay eager
    // (seg_may_be_sparse: the I-cache reason).
    bool sparse = seg_may_be_sparse(seg);
    struct Burrow *burrow = sparse ? burrow_create_anon_lazy(g.size)
                                   : burrow_create_anon(g.size);
    if (!burrow)                               return -1;

    // L-7 F4: hoisted to function scope so the map-in failure below -- which is
    // outside the sparse block but still after the populate -- can unwind the
    // same charge. 0 on the eager path, where nothing was charged.
    size_t nfile = 0;

    if (sparse) {
        nfile = seg_file_pages(&g, seg->filesz);
        if (nfile > 0 &&
            burrow_lazy_populate(as, exempt, burrow, 0, nfile) != 0) {
            // No unwind here: populate is all-or-nothing and gives its OWN charge
            // back on every internal failure (burrow.c, "the whole charge,
            // matching the grant"). Only failures AFTER it succeeded owe one.
            burrow_unref(burrow);
            return -1;
        }
        // #149: the segment's bytes start `lead` into the Burrow; [0, lead)
        // stays demand-zero.
        if (seg->filesz > 0 &&
            lazy_write(burrow, (u64)g.lead, (const u8 *)blob + seg->file_offset,
                       seg->filesz) != 0) {
            return lazy_populate_unwind(as, burrow, nfile);
        }
        // No I-cache maintenance: seg_may_be_sparse admitted this segment precisely
        // because nothing here will ever be fetched as an instruction.
    }
    // Copy filesz bytes from blob[file_offset..] → burrow's first page (offset 0).
    // The BURROW's pages start at offset 0; vmaddr_start corresponds to
    // BURROW offset 0 (the segment is page-aligned).
    else if (seg->filesz > 0) {
        // #149: `+ g.lead` -- the Burrow starts at the page floor, so the
        // segment's own bytes land `lead` in. [0, lead) keeps its KP_ZERO.
        u8 *burrow_kva = (u8 *)pa_to_kva(page_to_pa(burrow->pages)) + g.lead;
        const u8 *src = (const u8 *)blob + seg->file_offset;
        for (size_t i = 0; i < seg->filesz; i++) {
            burrow_kva[i] = src[i];
        }
    }
    // Either way [filesz, size) reads as zero: eagerly from KP_ZERO, sparsely from
    // the demand-zero fault arm's KP_ZERO. Only WHEN the page is allocated differs.
    //
    // R7 F134 / REVENANT R-4 / #107: cache maintenance for EXECUTABLE segments.
    // Bytes were written via the data path (D-cache); EL0 fetches them via the
    // I-cache. ARM ARM B2.4.2 requires `dc cvau` + `ic ivau` per line + DSB/ISB,
    // or the I-cache may serve stale bytes left by a prior occupant of the same
    // recycled physical page.
    //
    // The span is g.size -- the WHOLE page-rounded segment, not filesz -- and the
    // call is NOT gated on filesz > 0: elf_load rejects only filesz > memsz, so a
    // PF_X segment with filesz == 0 (pure bss) loads and would otherwise map
    // executable with no maintenance at all (#107).
    //
    // Taking the burrow base directly rather than reusing the copy loop's
    // `burrow_kva` is deliberate: that pointer is offset by g.lead (#149) and is
    // scoped to the branch above, whereas the coherent span starts at the page
    // floor. And PF_X implies NOT sparse -- seg_may_be_sparse is exactly
    // `(flags & PF_X) == 0` -- so burrow->pages is always the eager allocation
    // here. Widening seg_may_be_sparse to admit PF_X would break that; the sync
    // would then run over a lazily-populated Burrow.
    if (seg->flags & PF_X)
        exec_make_exec_coherent((u8 *)pa_to_kva(page_to_pa(burrow->pages)), g.size);

    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = burrow_map_in(as, exempt, burrow, g.floor, g.size, prot);
    if (rc != 0) {
        return lazy_populate_unwind(as, burrow, nfile);   // nfile == 0 on the eager path
    }

    // Drop the caller-held handle. mapping_count=1 (from vma_alloc)
    // keeps the BURROW alive; freed at proc_free's vma_drain when
    // mapping_count drops to 0 (and handle_count is 0).
    burrow_unref(burrow);
    return 0;
}

// Map the user stack — a 1 MiB anonymous VMA at the fixed top-of-
// user-VA — plus a one-page guard VMA directly below it.
static int exec_map_user_stack(struct AddrSpace *as, bool exempt) {
    // LINEAGE L-4a: the stack is SPARSE -- it grows downward from the top by
    // demand-zero fault, the Linux model (#49). Every exec used to allocate and
    // zero the full 1 MiB (256 pages) whether or not the program ever descended
    // that far; now only the pages actually touched exist. exec_build_init_stack
    // populates the run its argv/auxv frame occupies at the top. The stack is RW
    // (never executable), so seg_may_be_sparse's I-cache reason does not arise.
    struct Burrow *burrow = burrow_create_anon_lazy(EXEC_USER_STACK_SIZE);
    if (!burrow)                               return -1;

    int rc = burrow_map_in(as, exempt, burrow, EXEC_USER_STACK_BASE,
                           EXEC_USER_STACK_SIZE, VMA_PROT_RW);
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
    if (vma_insert_in(as, exempt, guard) != 0) {
        vma_free(guard);
        return -1;
    }
    return 0;
}

// Map the shared vDSO clock page READ-ONLY into `p` at the fixed
// EXEC_USER_VDSO_BASE (docs/VDSO-DESIGN.md). Returns the mapped user VA for the
// AT_VDSO_CLOCK auxv entry, or 0 if the page is unavailable (boot OOM left
// vdso_clock_burrow() NULL) or the per-Proc VMA install fails (VMA SLUB OOM) --
// in which case exec omits the auxv entry and the reader falls back to
// SYS_CLOCK_GETTIME (best-effort; never a boot gate). Unlike the stack/segment
// Burrows, the vDSO Burrow is kernel-owned + shared + held forever (never
// unref'd), so burrow_map's mapping_count++ is the Proc's ONLY reference; the
// VVA's vma_drain drops it at teardown and the shared page never frees.
static u64 exec_map_vdso(struct AddrSpace *as, bool exempt) {
    struct Burrow *v = vdso_clock_burrow();
    if (!v)                                    return 0;
    if (burrow_map_in(as, exempt, v, EXEC_USER_VDSO_BASE, EXEC_USER_VDSO_SIZE,
                      VMA_PROT_READ) != 0)     return 0;
    return EXEC_USER_VDSO_BASE;
}

// #140: project a Proc's /env into the packed "NAME=VALUE\0" block the frame
// builder takes. The contract is in exec.h; what follows is why it is shaped
// this way.
//
// ONE env_render_environ CALL, not a size query followed by a render. The
// renderer walks the whole Env under e->lock and returns a single atomic
// snapshot, so one call is coherent by construction. Any two-call
// shape -- size-then-render, or render-then-probe-whether-there-is-more --
// opens a window in which a peer thread's /env write changes the answer
// between them, and the block would then be either silently short or wrongly
// refused. Peer threads sharing one p->env is the ordinary case, not a corner
// (Go drives many M-threads through one Proc).
//
// So the buffer is EXEC_ENV_DATA_MAX + 1 and a return of MORE than the bound
// is the unambiguous "did not fit". The extra byte is doing real work: asking
// for exactly the bound cannot distinguish an environment that is exactly that
// long from one that is longer, and treating the ambiguous case as too-big
// would reject a legal environment.
//
// The renderer is documented CROSS-PROC with its disclosure gate at the
// devproc call site ("do not add a second caller without carrying the gate").
// THIS IS THAT SECOND CALLER, and it does not carry the gate because there is
// nothing here to disclose: the projection is a Proc's OWN environment onto
// its OWN new stack, reaching no further than the Proc could reach by reading
// /env itself. The gate exists for devproc, where the reader and the Env's
// owner are different Procs; here they are the same one. env.h now says so.
int exec_stage_env(struct Proc *p, char **data_out, u32 *len_out,
                   u32 *count_out) {
    if (!data_out || !len_out || !count_out) return -(int)T_E_INVAL;
    *data_out = NULL;
    *len_out = 0;
    *count_out = 0;
    if (!p) return 0;                       // no Proc -> empty environment

    char *buf = kmalloc((size_t)EXEC_ENV_DATA_MAX + 1u, 0);
    if (!buf) return -(int)T_E_NOMEM;

    long got = env_render_environ(p, 0, buf, (long)EXEC_ENV_DATA_MAX + 1);
    if (got < 0)                       { kfree(buf); return -(int)T_E_INVAL; }
    if (got == 0)                      { kfree(buf); return 0; }  // empty env
    if ((u32)got > EXEC_ENV_DATA_MAX)  { kfree(buf); return -(int)T_E_2BIG; }

    // envc is the NUL count, and that is EXACT rather than an estimate: every
    // record the renderer emits ends in exactly one NUL, and it SKIPS any entry
    // whose value contains one (env_linux_encodable) -- which is the same
    // property that makes the block a legal envp at all.
    u32 count = 0;
    for (long i = 0; i < got; i++)
        if (buf[i] == '\0') count++;
    if (count > EXEC_ENV_MAX)          { kfree(buf); return -(int)T_E_2BIG; }
    // Every record is terminated, so the last byte is a NUL. The frame
    // builder's packing contract requires it and extincts otherwise, so it is
    // checked HERE, where it is still an error rather than a dead kernel.
    if (buf[got - 1] != '\0')          { kfree(buf); return -(int)T_E_INVAL; }

    *data_out  = buf;
    *len_out   = (u32)got;
    *count_out = count;
    return 0;
}

// Fill out[0..count-1] with the user VAs of the `count` NUL-terminated strings
// packed into data[0..data_len), the first beginning at base_va. False if the
// block does not hold EXACTLY `count` terminated records -- which both call
// sites treat as a kernel bug, because the syscall bodies validate the packing
// before a block ever reaches here.
//
// ONE implementation for argv and envp, and that is the point rather than a
// tidy-up: the frame carries two identically-packed vectors, and #140 existed
// in the shape it did because the surrounding code had been written twice.
static bool exec_fill_ptr_vector(u64 *out, u64 base_va,
                                 const char *data, u32 data_len, u32 count) {
    if (count == 0)             return data_len == 0;
    if (!data || data_len == 0) return false;
    u32 found = 0;
    u32 start = 0;
    for (u32 i = 0; i < data_len; i++) {
        if (data[i] != '\0') continue;
        if (found == count) return false;          // more records than promised
        out[found++] = base_va + start;
        start = i + 1;
    }
    // start == data_len iff the final byte was a NUL -- i.e. there is no
    // trailing unterminated tail that no pointer in the vector names.
    return found == count && start == data_len;
}

// Build the System V initial-process stack frame — argc / argv / envp / auxv
// plus the argv and envp strings regions — at the top of the user stack. See
// exec.h for the byte layout.
//
// ONE SHAPE since #140. It used to be two, a fixed 176-byte "no argv" frame
// and a variable argv-bearing one, and each wrote its own lone NULL for
// envp -- so the bug this function now fixes had two homes. The general
// arithmetic reduces to the old fixed frame exactly when argc and envc are
// both zero (exec.h pins that with a _Static_assert), so no caller that used
// to take the fixed shape can observe the merge.
//
// The stack BURROW (installed by exec_map_user_stack) is located via
// vma_lookup and written through the kernel direct map; offset 0 of the
// BURROW corresponds to EXEC_USER_STACK_BASE, so the frame's bytes land
// in the BURROW's last `frame_size` bytes. Returns the initial user sp —
// the user VA of the frame's `argc` word.
// Fill the System V auxv block: AT_PHDR/PHENT/PHNUM/PAGESZ, AT_HWCAP (the
// Linux-compatible feature word from g_hw_features.linux_hwcap — read-only
// after boot, so a plain read is coherent), AT_RANDOM, AT_ENTRY, the OPTIONAL
// AT_VDSO_CLOCK (only when vdso_va != 0 — the page mapped), then the AT_NULL
// terminator. `a` has room for EXEC_INIT_AUXV_COUNT (9) entries; with no
// vDSO it writes 8 and the 9th 16-byte slot stays the caller-zeroed padding
// before the AT_RANDOM block (the reader stops at the AT_NULL terminator). Both
// frame shapes route through here, so the entry set cannot diverge.
//
// AT_ENTRY (D-2) is the image's FINAL entry — img->entry, which already
// carries the PIE bias. Unconditional: an ET_EXEC's entry is as real as a
// PIE's, and a tag that appears only sometimes is the harder contract to
// reason about. See elf.h for what it is and is not load-bearing for.
_Static_assert(EXEC_INIT_AUXV_COUNT == 9,
               "exec_fill_auxv reserves room for exactly 9 auxv entries "
               "(AT_PHDR, AT_PHENT, AT_PHNUM, AT_PAGESZ, AT_HWCAP, AT_RANDOM, "
               "AT_ENTRY, AT_VDSO_CLOCK, AT_NULL) — keep this in sync with the macro");
static void exec_fill_auxv(u64 *a, u64 phdr_va, u64 phent, u64 phnum,
                           u64 rand_va, u64 vdso_va, u64 entry_va) {
    *a++ = AT_PHDR;   *a++ = phdr_va;
    *a++ = AT_PHENT;  *a++ = phent;
    *a++ = AT_PHNUM;  *a++ = phnum;
    *a++ = AT_PAGESZ; *a++ = PAGE_SIZE;
    *a++ = AT_HWCAP;  *a++ = g_hw_features.linux_hwcap;
    *a++ = AT_RANDOM; *a++ = rand_va;
    *a++ = AT_ENTRY;  *a++ = entry_va;
    if (vdso_va) { *a++ = AT_VDSO_CLOCK; *a++ = vdso_va; }
    *a++ = AT_NULL;   *a++ = 0;
}

// `vdso_va` is the user VA of the mapped vDSO clock page (from exec_map_vdso),
// or 0 if it could not be mapped (-> no AT_VDSO_CLOCK; reader falls back).
// Returns the initial user sp, or 0 on failure -- LINEAGE L-4a made this failable
// (the stack Burrow is sparse now, so the frame's pages must be populated and the
// frame staged, both of which can hit OOM). 0 is unambiguous: a real sp is always
// just below EXEC_USER_STACK_TOP.
static u64 exec_build_init_stack(struct AddrSpace *as, bool exempt,
                                 const struct elf_image *img,
                                 const char *argv_data, u32 argv_data_len,
                                 u32 argc,
                                 const char *env_data, u32 env_data_len,
                                 u32 envc, u64 vdso_va) {
    // Structural invariants. The syscall bodies have already checked all of
    // these -- this entry is reached from several of them and owns the contract
    // too, so they are re-asserted as defense-in-depth. Reaching one is a
    // KERNEL bug (a front end that packed a block wrong), which is why they
    // extinct rather than fail: a frame whose pointer vector disagrees with its
    // strings region hands EL0 a wild pointer, and failing quietly here would
    // move the crash into the new image with nothing naming the cause.
    if (argc > 0) {
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
    } else if (argv_data_len != 0) {
        extinction("exec_build_init_stack: argv_data_len with argc == 0");
    }
    // The env bounds live in exec.h beside the frame arithmetic that justifies
    // them, so unlike argv's they are named rather than mirrored literals.
    if (envc > 0) {
        if (env_data_len == 0 || !env_data)
            extinction("exec_build_init_stack: envc > 0 with no env_data");
        if (envc > EXEC_ENV_MAX)
            extinction("exec_build_init_stack: envc exceeds bound");
        if (env_data_len > EXEC_ENV_DATA_MAX)
            extinction("exec_build_init_stack: env_data_len exceeds bound");
        if (env_data[env_data_len - 1] != '\0')
            extinction("exec_build_init_stack: env_data not NUL-terminated");
    } else if (env_data_len != 0) {
        extinction("exec_build_init_stack: env_data_len with envc == 0");
    }

    // exec_map_user_stack returned 0 → the stack VMA + its BURROW exist.
    struct Vma *sv = vma_lookup_in(as, EXEC_USER_STACK_BASE);
    if (!sv || !sv->burrow)
        extinction("exec_build_init_stack: stack VMA missing");

    // Frame layout -- exec.h carries the picture and owns the arithmetic, so
    // the two cannot drift:
    //   structured  = argc + argv[]+NULL + envp[]+NULL + auxv
    //   R           = round_up(structured, 16)      the AT_RANDOM block
    //   strings     = R + 16                        argv's, then envp's
    //   frame_size  = round_up(strings + argv_data_len + env_data_len, 16)
    u64 structured            = EXEC_INIT_STRUCTURED(argc, envc);
    u64 random_offset_from_sp = (structured + 15u) & ~15ull;
    u64 frame_size            = EXEC_INIT_FRAME_SIZE(argc, envc,
                                                     argv_data_len, env_data_len);

    u64 sp        = EXEC_USER_STACK_TOP - frame_size;
    u64 frame_off = EXEC_USER_STACK_SIZE - frame_size;   // its offset in the Burrow

    // LINEAGE L-4a: the stack Burrow is sparse, so there is no contiguous kva to
    // lay the frame out in. Build it in a kernel scratch buffer -- leaving every
    // field write below byte-identical -- and copy it in whole at the end. The
    // scratch is transient (freed before this returns) and bounded by
    // EXEC_INIT_STACK_MAX_SIZE (~104 KiB worst case since #140 added the
    // environment; 176 bytes with no argv and an empty env), against the 1 MiB
    // the eager stack used to cost unconditionally.
    u8 *frame = kzalloc((size_t)frame_size, 0);
    if (!frame) return 0;

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
    //
    // The two strings regions are ADJACENT and in this order -- argv's, then
    // envp's -- so each vector's base VA is where the previous region ended.
    // Nothing in the ABI requires that order; it is simply the one the layout
    // in exec.h documents, and the pointer vectors are what actually bind a
    // string to its slot.
    u64 *w              = (u64 *)frame;
    u64 argv_strings_va = sp + random_offset_from_sp + 16u;
    u64 env_strings_va  = argv_strings_va + (u64)argv_data_len;

    w[0] = (u64)argc;
    if (!exec_fill_ptr_vector(&w[1], argv_strings_va,
                              argv_data, argv_data_len, argc))
        extinction("exec_build_init_stack: NUL count != argc post-validation");
    w[1 + argc] = 0;                         // argv[argc] = NULL terminator

    if (!exec_fill_ptr_vector(&w[2 + argc], env_strings_va,
                              env_data, env_data_len, envc))
        extinction("exec_build_init_stack: NUL count != envc post-validation");
    w[2 + argc + envc] = 0;                  // envp[envc] = NULL terminator

    // auxv at w[3 + argc + envc ..]; EXEC_INIT_AUXV_COUNT entries (16 u64s)
    // reserved, which is exactly what EXEC_INIT_STRUCTURED accounts for, so the
    // block ends precisely where the AT_RANDOM padding begins. The trailing
    // slots beyond the AT_NULL the helper writes stay zero.
    exec_fill_auxv(&w[3 + argc + envc], phdr_va, phent, phnum,
                   sp + random_offset_from_sp, vdso_va, img->entry);

    // Copy both strings regions in. Bounded by the frame_size arithmetic above,
    // which reserved exactly argv_data_len + env_data_len bytes after the
    // AT_RANDOM block.
    u8 *strings_dst = frame + random_offset_from_sp + 16u;
    for (u32 i = 0; i < argv_data_len; i++)
        strings_dst[i] = (u8)argv_data[i];
    for (u32 i = 0; i < env_data_len; i++)
        strings_dst[(u64)argv_data_len + i] = (u8)env_data[i];

    u8 *rand_dst = frame + random_offset_from_sp;
    for (size_t i = 0; i < sizeof(rand); i++) rand_dst[i] = rand[i];

    // LINEAGE L-4a: make the frame's pages resident and copy it in. The run is the
    // page holding the frame's first byte through the top of the stack -- the frame
    // is 16-aligned but not page-aligned, so its first byte generally lands
    // mid-page. Everything BELOW that page stays sparse and demand-zeroes as the
    // program descends.
    size_t first = (size_t)(frame_off / PAGE_SIZE);
    size_t n     = (size_t)(EXEC_USER_STACK_SIZE / PAGE_SIZE) - first;
    int rc = burrow_lazy_populate(as, exempt, sv->burrow, first, n);
    if (rc == 0) {
        rc = lazy_write(sv->burrow, frame_off, frame, (size_t)frame_size);
        // L-7 F4: the populate succeeded, so its charge for the whole run is
        // outstanding and a lazy_write failure owes it back. Deliberately inside
        // this arm: the populate-failure path already gave its own charge back,
        // and uncharging there as well would UNDER-count.
        if (rc != 0) lazy_populate_uncharge(as, n);
    }
    kfree(frame);
    if (rc != 0) return 0;      // caller disposes the partially-built address space

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
static int exec_setup_argv_body(struct AddrSpace *as, bool exempt,
                                const void *blob, size_t blob_size,
                                const char *argv_data, u32 argv_data_len,
                                u32 argc,
                                const char *env_data, u32 env_data_len,
                                u32 envc,
                                u64 *entry_out, u64 *sp_out) {
    if (!as || !blob || !entry_out || !sp_out) return -1;
    // The target must be CLEAN. For the spawn paths that is a freshly rforked
    // child's address space; for execve it is the detached one being built.
    if (as->vmas != NULL)                      return -1;

    // argv invariants (defense-in-depth — the syscall body has already
    // checked these, but exec_setup_with_argv is exported to other
    // callers and the contract is owned here too).
    if (argc > 0) {
        if (argv_data_len == 0 || !argv_data)  return -1;
        if (argv_data[argv_data_len - 1] != '\0') return -1;
    } else {
        if (argv_data_len != 0)                return -1;
    }
    // The same shape for the environment (#140): the builder extincts on a
    // mismatch, so a badly-packed block must be caught here, where it is still
    // an error.
    if (envc > 0) {
        if (env_data_len == 0 || !env_data)    return -1;
        if (envc > EXEC_ENV_MAX)               return -1;
        if (env_data_len > EXEC_ENV_DATA_MAX)  return -1;
        if (env_data[env_data_len - 1] != '\0') return -1;
    } else {
        if (env_data_len != 0)                 return -1;
    }

    struct elf_image img;
    int r = elf_load(blob, blob_size, &img);
    if (r != ELF_LOAD_OK)                      return -1;

    // Map each PT_LOAD segment.
    for (int i = 0; i < img.n_segments; i++) {
        if (exec_map_segment(as, exempt, blob, &img.segments[i]) != 0) {
            // Partial state: caller disposes the target address space.
            return -1;
        }
    }

    // Allocate user stack.
    if (exec_map_user_stack(as, exempt) != 0) {
        return -1;
    }

    // Map the shared vDSO clock page RO (best-effort; 0 -> no AT_VDSO_CLOCK).
    u64 vdso_va = exec_map_vdso(as, exempt);

    // Build the System V startup frame (argc / argv / envp / auxv +
    // strings region under Shape B) at the top of the user stack;
    // *sp_out points at its `argc` word.
    *entry_out = img.entry;
    *sp_out    = exec_build_init_stack(as, exempt, &img, argv_data, argv_data_len,
                                       argc, env_data, env_data_len, envc,
                                       vdso_va);
    if (*sp_out == 0)                          return -1;   // L-4a: frame OOM
    return 0;
}

// The Proc-taking gate the two blob entries share: reject kproc, and resolve the
// Proc to its own (attached) address space + I-32 exemption. execve does NOT go
// through here -- it targets a detached address space and supplies its own.
//
// THE ENVIRONMENT COMES FROM THE PROC (#140), and that is the rule for every
// Proc-taking exec entry without exception -- the blob entries here and
// exec_setup_from_spoor below. A caller that has an environment of its own to
// impose does not come through a Proc-taking entry at all; it goes to
// exec_load_into with an explicit block. One rule, no per-entry exceptions to
// remember, and the reason envp was empty for years is that there was no rule.
static int exec_setup_for_proc(struct Proc *p,
                               const void *blob, size_t blob_size,
                               const char *argv_data, u32 argv_data_len,
                               u32 argc, u64 *entry_out, u64 *sp_out) {
    if (!p)                     return -1;
    if (p->magic != PROC_MAGIC) return -1;
    if (!p->as)                 return -1;     // kproc rejected

    char *env_data = NULL;
    u32 env_len = 0, envc = 0;
    if (exec_stage_env(p, &env_data, &env_len, &envc) != 0) return -1;

    int rc = exec_setup_argv_body(p->as, proc_resource_exempt(p),
                                  blob, blob_size, argv_data, argv_data_len, argc,
                                  env_data, env_len, envc,
                                  entry_out, sp_out);
    kfree(env_data);            // copied into the frame; no-op on NULL
    return rc;
}

int exec_setup(struct Proc *p, const void *blob, size_t blob_size,
               u64 *entry_out, u64 *sp_out) {
    return exec_setup_for_proc(p, blob, blob_size,
                               /*argv_data=*/NULL, /*argv_data_len=*/0u,
                               /*argc=*/0u,
                               entry_out, sp_out);
}

int exec_setup_with_argv(struct Proc *p, const void *blob, size_t blob_size,
                         const char *argv_data, u32 argv_data_len, u32 argc,
                         u64 *entry_out, u64 *sp_out) {
    return exec_setup_for_proc(p, blob, blob_size,
                               argv_data, argv_data_len, argc,
                               entry_out, sp_out);
}

// =============================================================================
// REVENANT R-4: the file-backed production exec path.
// =============================================================================
//
// docs/REVENANT.md §4.2 + §4.6. exec_setup_from_spoor reads ONLY the ELF
// header+phdrs from the pinned executable, then maps each PT_LOAD: NON-WRITABLE
// segments (R+X text AND R-only rodata -- #45) whose memsz fits within
// file-backed pages are SHARED file-backed via the Image cache (demand-paged by
// the R-2 fault arm); writable data (and degenerate whole-bss-page tails) is a
// private eager-copied anonymous segment. This retires the whole-binary slurp
// -- a binary of any size execs.

// map_file_backed -- a NON-WRITABLE PT_LOAD (R+X text or R-only rodata, #45 /
// REVENANT §4.6) as a SHARED file-backed segment. Gated by the caller to
// round_up(filesz) == round_up(memsz) (every mapped page has a file page behind
// it; the last partial page's tail is the file's zero padding between
// page-aligned segments, or EOF -> the fault arm's KP_ZERO). BORROWS exe;
// spoor_refs a fresh ref for the consuming Image lookup.
static int map_file_backed(struct AddrSpace *as, bool exempt, struct Spoor *exe,
                           const struct elf_load_segment *seg) {
    struct seg_geom g;
    if (seg_geometry(seg, &g) != 0)         return -1;

    // #149: this arm REQUIRES burrow-offset-0 == seg->vaddr. The R-2 fault arm
    // derives each page's file position as
    //     v->file_offset + (burrow_byte_off & ~(PAGE_SIZE-1))     (fault.c)
    // which is only the segment's own bytes when the Burrow's offset 0 IS the
    // segment's start -- i.e. when vaddr and file_offset are both page-aligned.
    // Rather than teach the audited fault arm + the qid-keyed Image cache about
    // an intra-page lead, exec_load_into's file_shareable gate keeps unaligned
    // segments off this arm entirely, so the identity REVENANT §4.6 assumes
    // stays true BY CONSTRUCTION instead of by assumption. This re-check is the
    // defense-in-depth for a future direct caller; it costs two compares.
    // Measured: 0 of 20 ELFs in a stock Alpine rootfs has an unaligned
    // non-writable PT_LOAD, so the eager fallback is never taken in practice.
    if (g.lead != 0)                        return -1;
    if (seg->file_offset & (PAGE_SIZE - 1)) return -1;
    size_t size = g.size;

    // image_lookup_or_create CONSUMES one spoor ref (adopts on miss / clunks on
    // hit). The thunk keeps the borrowed `exe`, so hand the lookup a FRESH ref;
    // on a NULL return it consumed nothing -> drop the fresh ref.
    //
    // #194: the sampled size lets the fault arm refuse pages wholly past EOF,
    // which closes the lying-ELF mint (a phdr claiming filesz beyond the real
    // file end used to demand-zero the difference, uncharged). exec ADMITS
    // UNKNOWN -- the only size-less backing Dev is the baked ramfs, which no
    // guest can write, so a lying ELF cannot exist there.
    spoor_ref(exe);
    struct Burrow *b = image_lookup_or_create(exe, seg->file_offset, seg->filesz,
                                              (seg->flags & PF_X) != 0,
                                              spoor_file_size(exe));
    if (!b) { spoor_clunk(exe); return -1; }

    // The segment's own ELF prot: R+X for text, R-only (XN) for rodata. W^X
    // (I-12) holds by construction: the gate admits only NOT-PF_W segments, so
    // nothing mapped here is ever writable. Each FILE Burrow backs exactly ONE
    // segment at ONE prot (the Image keys per-segment), which is what keeps the
    // fault arm's freq->exec-gated I-cache sync sound (REVENANT §4.6).
    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = burrow_map_in(as, exempt, b, seg->vaddr, size, prot);
    // Drop the caller's handle ref. On success mapping_count + the cache's
    // strong ref keep the image alive; on map failure the image stays validly
    // cached (idle) for a later exec -- never leaked.
    burrow_unref(b);
    return rc == 0 ? 0 : -1;
}

// map_eager_from_file -- a non-shared PT_LOAD eager-copied from the file into a
// PRIVATE anonymous Burrow: writable data (R+W), or a rare non-writable segment
// whose memsz extends past the file's last page (whole bss pages the file-backed
// path cannot zero). Since #45 rodata (R-only) rides map_file_backed instead.
// filesz bytes are dev->read; the [filesz, size) tail stays zero (KP_ZERO) =
// .bss. No userspace file-backed writable mapping is ever created (I-36-4).
static int map_eager_from_file(struct AddrSpace *as, bool exempt, struct Spoor *exe,
                               const struct elf_load_segment *seg) {
    // #149: the arm that carries an unaligned segment. Unlike map_file_backed
    // this one COPIES bytes at an explicit dev->read offset, so it has no
    // burrow-offset-0 identity to preserve and needs no alignment gate on
    // either vaddr or file_offset.
    struct seg_geom g;
    if (seg_geometry(seg, &g) != 0) {
        exec_report_fail("degenerate PT_LOAD (memsz 0 or wrapping end) at vaddr",
                         seg->vaddr);
        return -1;
    }
    size_t size = g.size;

    // LINEAGE L-4a: the same sparse/eager split as exec_map_segment. This is the
    // production arm, and it is where the measured cost was: corvus's RW PT_LOAD is
    // FileSiz 128 B / MemSiz 24 MiB, and sizing by MEMSZ made every corvus exec
    // allocate AND zero a 32 MiB order-13 block for 128 bytes of data (#130).
    bool sparse = seg_may_be_sparse(seg);
    struct Burrow *b = sparse ? burrow_create_anon_lazy(size)
                              : burrow_create_anon(size);
    if (!b)                                  return -1;

    // L-7 F4: function-scoped so the map-in failure below can unwind the same
    // charge; 0 on the eager path, where nothing was charged.
    size_t nfile = 0;

    if (sparse) {
        // Populate only the file-backed head; read into it a page at a time (the
        // slots are separate order-0 pages, so there is no contiguous span to read
        // into). Byte-identical to the eager read: same offsets, same total, and
        // the [filesz, size) tail is still zero -- it is simply not allocated yet.
        nfile = seg_file_pages(&g, seg->filesz);
        if (nfile > 0 &&
            burrow_lazy_populate(as, exempt, b, 0, nfile) != 0) {
            burrow_unref(b);   // populate self-unwinds its charge; nothing owed
            return -1;
        }
        size_t got = 0;
        while (got < seg->filesz) {
            // #149: the destination walks from Burrow offset `lead`, not 0. The
            // SOURCE offset (seg->file_offset + got) is unchanged -- the leading
            // slack is not read from the file, it stays demand-zero.
            size_t dst_off = g.lead + got;
            u8 *slot_kva = (u8 *)burrow_lazy_slot_kva(b, dst_off / PAGE_SIZE);
            // L-7 F4: past the populate, so these three owe the charge back.
            if (!slot_kva) return lazy_populate_unwind(as, b, nfile);
            size_t in_page = dst_off % PAGE_SIZE;
            size_t want    = (size_t)PAGE_SIZE - in_page;
            if (want > seg->filesz - got) want = (size_t)(seg->filesz - got);
            long n = exe->dev->read(exe, slot_kva + in_page, (long)want,
                                    (s64)(seg->file_offset + got));
            // #811-interruptible / I/O error -- the reachable one, on a real 9P read
            if (n < 0)  return lazy_populate_unwind(as, b, nfile);
            if (n == 0) break;                             // short read (caught below)
            got += (size_t)n;
        }
        // truncated -> no partial map
        if (got != seg->filesz) return lazy_populate_unwind(as, b, nfile);
        // No I-cache maintenance: seg_may_be_sparse admitted this segment precisely
        // because nothing here will ever be fetched as an instruction.
    } else if (seg->filesz > 0) {
        // #149: `+ g.lead` -- the contiguous Burrow starts at the page floor.
        u8 *kva = (u8 *)pa_to_kva(page_to_pa(b->pages)) + g.lead;
        size_t got = 0;
        while (got < seg->filesz) {
            long n = exe->dev->read(exe, kva + got, (long)(seg->filesz - got),
                                    (s64)(seg->file_offset + got));
            if (n < 0)  { burrow_unref(b); return -1; }   // #811-interruptible / I/O error
            if (n == 0) break;                             // short read (caught below)
            got += (size_t)n;
        }
        if (got != seg->filesz) { burrow_unref(b); return -1; }   // truncated -> no partial map
    }
    // [filesz, size) reads as zero either way (KP_ZERO eagerly, or from the
    // demand-zero fault arm) = .bss.
    //
    // #107: a PF_X segment routed here (memsz extends past the file pages) still
    // executes its loaded bytes -> I-cache maintenance, over the whole
    // page-rounded span and NOT gated on filesz > 0. This is precisely the path
    // that receives a segment whose memsz exceeds its filesz, so the un-synced
    // tail is the COMMON case here rather than an edge one.
    //
    // Base + g.size, not the copy loop's `kva`: that pointer is offset by g.lead
    // (#149) and scoped to the branch above. PF_X implies NOT sparse
    // (seg_may_be_sparse is `(flags & PF_X) == 0`), so b->pages is always the
    // eager allocation here -- widening that predicate would break this.
    if (seg->flags & PF_X)
        exec_make_exec_coherent((u8 *)pa_to_kva(page_to_pa(b->pages)), g.size);

    u32 prot = vma_prot_for_elf(seg->flags);
    int rc = burrow_map_in(as, exempt, b, g.floor, size, prot);
    // L-7 F4: on FAILURE the charge is owed back (nfile == 0 on the eager path,
    // where none was taken). On SUCCESS the mapping keeps the pages and the
    // charge correctly describes them, so only the construction handle drops.
    if (rc != 0) return lazy_populate_unwind(as, b, nfile);
    burrow_unref(b);
    return 0;
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

// LINEAGE L-2: the file-backed load, addressed by AddrSpace. This is the whole
// of exec_setup_from_spoor's body except the Proc-side name/exe_path stamps,
// which are deliberately NOT here -- they mutate the Proc, and execve must be
// able to fail with the caller completely untouched, so its caller applies them
// only at the commit point. The spawn wrapper below keeps applying them inline
// (its Proc is a fresh child that is discarded on failure, so the distinction
// cannot be observed there).
int exec_load_into(struct AddrSpace *as, bool exempt,
                   struct Spoor *exe, size_t exe_size,
                   const char *argv_data, u32 argv_data_len, u32 argc,
                   const char *env_data, u32 env_data_len, u32 envc,
                   u64 *entry_out, u64 *sp_out) {
    if (!as || !exe || !entry_out || !sp_out)  return -1;
    if (as->vmas != NULL)                      return -1;     // clean target only
    if (!exe->dev || !exe->dev->read)          return -1;
    if (exe_size == 0 || exe_size > EXEC_FILE_MAX) return -1;

    // argv invariants (defense-in-depth; the syscall body already checked them,
    // but the contract is owned here too -- this entry is exported).
    if (argc > 0) {
        if (argv_data_len == 0 || !argv_data)      return -1;
        if (argv_data[argv_data_len - 1] != '\0')  return -1;
    } else {
        if (argv_data_len != 0)                    return -1;
    }
    // ...and the same for the environment (#140). The builder extincts on a
    // mismatch, so the bad block must be refused here, where it is an error.
    if (envc > 0) {
        if (env_data_len == 0 || !env_data)        return -1;
        if (envc > EXEC_ENV_MAX)                   return -1;
        if (env_data_len > EXEC_ENV_DATA_MAX)      return -1;
        if (env_data[env_data_len - 1] != '\0')    return -1;
    } else {
        if (env_data_len != 0)                     return -1;
    }

    // 1. Read + parse ONLY the ELF header + phdrs (a few KB), not the whole
    //    binary. elf_load validates segment extents against the real file size.
    size_t hdr_got = 0;
    void *hdr = exec_read_header(exe, &hdr_got);
    if (!hdr)                                  return -1;
    struct elf_image img;
    int r = elf_load(hdr, exe_size, &img);
    // VIVARIUM section 12.1 rule 4: the ELF brand is ADVISORY -- it may never
    // decide a phenotype (rule 3: outside a vivarium the answer is always
    // native), but an obvious mismatch should earn "a diagnostic and a clean
    // failure, not a silent mis-decode". This is that diagnostic, and it is
    // the hint function's first caller. Consulted ONLY on an already-failed
    // load, so it can never change an outcome -- it only explains one. The
    // failure is the pre-existing dynamic-binary reject (v1.0 is static-only,
    // section 9's OUT list); before this, a user got a bare -1.
    if (r == ELF_LOAD_HAS_INTERP || r == ELF_LOAD_HAS_DYNAMIC) {
        if (elf_brand_hint(hdr, hdr_got) == ELF_BRAND_LINUX_LIKELY) {
            uart_puts("exec: dynamic Linux binary rejected -- Thylacine v1.0 "
                      "runs statically-linked ELF only (VIVARIUM section 9)\n");
        }
    }
    kfree(hdr);
    if (r != ELF_LOAD_OK) {
        // #149: name the code. The enum's comments in elf.h are the decode
        // table (-15 HAS_INTERP, -16 RWX_REJECTED, -23 HAS_DYNAMIC, ...); a
        // bare -1 out of a thunk that already returned a pid told the operator
        // nothing at all.
        exec_report_fail("elf_load refused this binary -- negated ELF_LOAD code",
                         (u64)(-r));
        return -1;
    }

    // 2. Map each PT_LOAD. Non-writable segments (R+X text, R-only rodata --
    //    #45) whose memsz fits within file-backed pages are SHARED file-backed
    //    (demand-paged); writable data is a private eager-copied anon segment.
    //    #149: a segment maps from its page-aligned FLOOR (seg_geometry), so an
    //    unaligned vaddr is no longer refused -- but it must not take the
    //    file-backed arm, whose fault path and Image cache both require
    //    burrow-offset-0 == seg->vaddr.
    for (int i = 0; i < img.n_segments; i++) {
        const struct elf_load_segment *seg = &img.segments[i];
        struct seg_geom g;
        if (seg_geometry(seg, &g) != 0) {
            exec_report_fail("degenerate PT_LOAD (memsz 0 or wrapping end) "
                             "at vaddr", seg->vaddr);
            return -1;
        }

        // #149: refuse a segment whose first page is ALREADY mapped. Two
        // PT_LOADs sharing a page is representable in ELF and unsatisfiable
        // here: the page would have to carry the earlier segment's bytes at the
        // earlier segment's permissions AND this one's, and for the common
        // text-then-data pair that is W AND X in one PTE -- exactly what I-12
        // forbids. Linux resolves it by letting the later mapping win the page
        // (so trailing text silently loses X); we refuse instead, because a
        // loader that quietly drops execute permission from a page of code is
        // the kind of silent-wrong this whole task exists to eliminate.
        // Measured: 0 of 20 ELFs in a stock Alpine rootfs shares a page --
        // aarch64's 64 KiB p_align leaves segments pages apart. vma_insert
        // would reject the overlap anyway; this names it.
        if (vma_lookup_in(as, g.floor) != NULL) {
            exec_report_fail("PT_LOAD shares a page with an earlier segment "
                             "(I-12 forbids the W+X union) at vaddr", seg->vaddr);
            return -1;
        }

        // file_shareable iff PF_R AND NOT PF_W (R+X text, R-only rodata -- #45 /
        // REVENANT §4.6; text byte-identical since elf_load rejects W|X) AND
        // round_up(filesz) == round_up(memsz) (no whole bss page beyond the
        // file). The PF_R requirement (#45 audit F2) keeps a no-access (flags==0)
        // PT_LOAD on the eager path -- it can never be read/faulted, so caching
        // it would only pin an idle Image slot + spoor ref. vaddr is
        // page-aligned, so the vaddr-relative rounded ends compare the rounded
        // sizes; a 0 from round_up_page is overflow -> fall to the eager path
        // (which re-rejects). PF_W MUST stay eager: a file-backed writable
        // mapping violates I-36-4 + the §6.5 refusal.
        u64 fend = round_up_page(seg->vaddr + seg->filesz);
        u64 mend = round_up_page(seg->vaddr + seg->memsz);
        // #149 adds the two alignment terms: the file-backed arm is the one
        // that needs burrow-offset-0 == seg->vaddr, so gating HERE keeps that
        // identity true by construction rather than by assumption. An unaligned
        // non-writable segment degrades to the eager arm -- private instead of
        // shared, so it loads correctly and merely forgoes the Image cache.
        // Fail-SAFE by design: the arm that cannot represent it never sees it.
        bool file_shareable = (seg->flags & PF_R) && !(seg->flags & PF_W) &&
                              g.lead == 0 &&
                              (seg->file_offset & (PAGE_SIZE - 1)) == 0 &&
                              fend != 0 && fend == mend;

        int rc = file_shareable ? map_file_backed(as, exempt, exe, seg)
                                : map_eager_from_file(as, exempt, exe, seg);
        if (rc != 0) {
            exec_report_fail(file_shareable
                                 ? "file-backed PT_LOAD map failed at vaddr"
                                 : "eager PT_LOAD map failed at vaddr",
                             seg->vaddr);
            return -1;   // partial -> caller disposes target
        }
    }

    // 3. User stack + the vDSO clock page + the System V startup frame (reads
    //    img metadata, not the file -- AT_PHDR resolves into the first mapped
    //    segment's VA; AT_VDSO_CLOCK into the RO clock page).
    if (exec_map_user_stack(as, exempt) != 0)  return -1;
    u64 vdso_va = exec_map_vdso(as, exempt);   // best-effort; 0 -> no AT_VDSO_CLOCK
    *entry_out = img.entry;
    *sp_out    = exec_build_init_stack(as, exempt, &img, argv_data, argv_data_len,
                                       argc, env_data, env_data_len, envc,
                                       vdso_va);
    if (*sp_out == 0)                          return -1;   // L-4a: frame OOM
    return 0;
}

// The spawn entry: load into the Proc's own (fresh, empty) address space and
// apply the two Proc-side stamps. Every SYS_SPAWN_* thunk and the boot init-load
// come through here; execve does not (see exec_load_into's header).
int exec_setup_from_spoor(struct Proc *p, struct Spoor *exe, size_t exe_size,
                          const char *argv_data, u32 argv_data_len, u32 argc,
                          u64 *entry_out, u64 *sp_out) {
    if (!p || !exe)                            return -1;
    if (p->magic != PROC_MAGIC)                return -1;
    if (!p->as)                                return -1;     // kproc rejected

    // prowl-1 (PROWL-DESIGN.md section 3.2): stamp the process name from the
    // resolved binary path -- the UNFORGEABLE identity (the basename of what
    // actually execs, not caller-controlled argv[0]). exe->path is the #66
    // namespace name of the resolved Spoor ("/bin/corvus" -> "corvus"), fail-soft
    // NULL on an OOM path-alloc (I-33) -> the name stays "" (the formatters show
    // "?"). Runs in the CHILD's context (p == the execing Proc), before it reaches
    // EL0 -- no concurrent reader observes a torn stamp.
    if (exe->path)
        proc_set_name(p, exe->path->s, (size_t)exe->path->len);

    // #140: the spawn paths project the CHILD's own environment, and it is
    // already the right one -- rfork_internal ran env_clone_into before this
    // thunk started, so p->env is the inherited copy. No parent access is
    // needed or wanted; a spawn that wants a DIFFERENT environment sets it
    // through /env on the child, which is what the namespace is for.
    char *env_data = NULL;
    u32 env_len = 0, envc = 0;
    if (exec_stage_env(p, &env_data, &env_len, &envc) != 0) {
        exec_report_fail("environment does not fit the exec frame, pid",
                         (u64)p->pid);
        return -1;
    }

    int rc = exec_load_into(p->as, proc_resource_exempt(p), exe, exe_size,
                            argv_data, argv_data_len, argc,
                            env_data, env_len, envc, entry_out, sp_out);
    kfree(env_data);            // copied into the frame; no-op on NULL
    if (rc != 0) {
        // #149: the pid line. Every SYS_SPAWN_* thunk reaches exec through
        // here, and all three answer a failure with a bare exits("fail-exec")
        // -- so without this the parent sees a healthy positive pid and a child
        // that exited 1 having executed nothing, with no log line naming it.
        // exec_load_into has already said WHY; this says WHO.
        exec_report_fail("load failed, terminating pid", (u64)p->pid);
        return -1;
    }

    // VIVARIUM V-4a-0: record the executable's namespace name for
    // /proc/<pid>/exe. `exe->path` is the #66 Path stalk built while
    // exec_resolve_from_namespace resolved this binary -- the only place in
    // the system that still knows what the running program is CALLED (the
    // Image cache is qid-keyed, the text Burrow anonymous). Set LAST, only
    // on the success path, so a Proc never advertises a binary it failed to
    // load. NULL-tolerant both ways: the blob init-load path (`/joey`,
    // which predates any namespace) and a #66 alloc failure both leave it
    // NULL, which renders as an empty file and fails no exec (I-33).
    //
    // Unlocked by construction: this runs in the exec'ing Proc's own
    // context, before it has a second thread, and the field's only other
    // writers are its own rfork (which precedes this) and its proc_free.
    proc_set_exe_path(p, exe->path);
    return 0;
}
