// arch/arm64/fault.c — page-fault dispatcher (P3-C / P3-Dc).
//
// Decodes ESR_EL1 / FAR_EL1 / ELR_EL1 into a structured `fault_info`
// and dispatches to handlers. At P3-Dc the user-mode path resolves
// faults via demand paging: `userland_demand_page` looks up the VMA
// covering FAR, validates the access type vs VMA prot, resolves to
// the backing BURROW PA, and installs an L3 PTE in the per-Proc TTBR0
// tree (mmu_install_user_pte). Other fault paths still extinct
// (kstack guard / W^X / unhandled kernel translation).
//
// Per ARCHITECTURE.md §12 (exception model) + §16 (process address
// space) + §28 invariants I-7 + I-12.

#include "fault.h"

#include "kaslr.h"
#include "mmu.h"

#include <thylacine/dev.h>           // REVENANT R-2: struct Dev (spoor->dev->read) for the FILE fault arm
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>         // REVENANT R-2: struct Spoor for the pinned backing Chan
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

#include "../../mm/phys.h"           // REVENANT R-2: alloc_pages/free_pages for the demand-read page

// Linker symbols — boot stack guard region + kernel image bounds.
extern char _boot_stack_guard[];
extern char _boot_stack_bottom[];
extern char _kernel_start[];
extern char _kernel_end[];

// #806: per-CPU re-entrancy guard for the kernel-fault dispatch. A fault
// taken WHILE handling a kernel fault (classically a wild current_thread()
// deref inside stack_guard_overflow_msg) would otherwise recurse one
// KERNEL_ENTRY frame per fault until the boot stack overflows -- making the
// real bug masquerade as "kernel stack overflow (boot-stack guard)". Per-CPU
// because faults are CPU-local; see arch_fault_handle.
static volatile bool g_in_kernel_fault[DTB_MAX_CPUS];

// =============================================================================
// ESR_EL1 decode constants (mirrors exception.c; centralized here for
// the dispatcher).
// =============================================================================

#define ESR_EC_SHIFT       26
#define ESR_EC(esr)        (((esr) >> ESR_EC_SHIFT) & 0x3F)

#define EC_INST_ABORT_LOWER 0x20    /* instruction abort from lower EL */
#define EC_INST_ABORT_SAME  0x21    /* instruction abort from current EL */
#define EC_DATA_ABORT_LOWER 0x24    /* data abort from lower EL */
#define EC_DATA_ABORT_SAME  0x25    /* data abort from current EL */

// ISS bit 9 = WnR (write/not-read). Only meaningful for data aborts.
#define ESR_ISS_WNR_BIT    9

// FSC = ISS[5:0] for data and instruction aborts.
#define FSC_MASK           0x3F

// Translation faults at L0..L3.
#define FSC_TRANS_FAULT_L0 0x04
#define FSC_TRANS_FAULT_L1 0x05
#define FSC_TRANS_FAULT_L2 0x06
#define FSC_TRANS_FAULT_L3 0x07

// Access-flag faults at L1..L3 (FEAT_HAFDBS — hardware access flag).
// Per ARM ARM D17.2.40, FSC values 0x09..0x0B.
#define FSC_ACCESS_FAULT_L1 0x09
#define FSC_ACCESS_FAULT_L2 0x0A
#define FSC_ACCESS_FAULT_L3 0x0B

// Permission faults at L1..L3.
#define FSC_PERM_FAULT_L1   0x0D
#define FSC_PERM_FAULT_L2   0x0E
#define FSC_PERM_FAULT_L3   0x0F

// =============================================================================
// fault_info_decode — pure decoder.
// =============================================================================

void fault_info_decode(u64 esr, u64 far, u64 elr, struct fault_info *out) {
    out->esr = esr;
    out->vaddr = far;
    out->elr = elr;

    u32 ec  = (u32)ESR_EC(esr);
    u32 fsc = (u32)(esr & FSC_MASK);

    out->ec  = ec;
    out->fsc = fsc;

    // Fault level: FSC[1:0] for translation/access/permission faults.
    out->fault_level = (u8)(fsc & 0x3);

    // EC distinguishes lower-EL (user) vs current-EL (kernel) aborts.
    out->from_user = (ec == EC_DATA_ABORT_LOWER || ec == EC_INST_ABORT_LOWER);

    // Instruction abort vs data abort.
    out->is_instruction = (ec == EC_INST_ABORT_LOWER || ec == EC_INST_ABORT_SAME);

    // WnR is bit 9 of ISS. ISS = ESR[24:0]. Only meaningful for data
    // aborts; for instruction aborts the bit is RES0 in the encoding so
    // the read happens to be 0 (which maps to "read", which is the
    // correct semantic — instruction fetches are reads).
    out->is_write = !out->is_instruction &&
                    ((esr >> ESR_ISS_WNR_BIT) & 1) != 0;

    // FSC classification.
    out->is_translation =
        fsc == FSC_TRANS_FAULT_L0 ||
        fsc == FSC_TRANS_FAULT_L1 ||
        fsc == FSC_TRANS_FAULT_L2 ||
        fsc == FSC_TRANS_FAULT_L3;

    out->is_access_flag =
        fsc == FSC_ACCESS_FAULT_L1 ||
        fsc == FSC_ACCESS_FAULT_L2 ||
        fsc == FSC_ACCESS_FAULT_L3;

    out->is_permission =
        fsc == FSC_PERM_FAULT_L1 ||
        fsc == FSC_PERM_FAULT_L2 ||
        fsc == FSC_PERM_FAULT_L3;
}

// =============================================================================
// Address-range classifiers.
// =============================================================================

static u64 high_va_to_pa(u64 high_va) {
    u64 high_va_kernel_start = kaslr_kernel_high_base();
    u64 pa_kernel_start = kaslr_kernel_pa_start();
    return high_va - high_va_kernel_start + pa_kernel_start;
}

static u64 sym_to_pa(const void *sym) {
    return high_va_to_pa((u64)(uintptr_t)sym);
}

// Returns the "kernel stack overflow" extinction message NAMING the guard
// region that contains `addr`, or NULL if `addr` is in no active guard.
// Three flavors: the boot CPU's _boot_stack guard (start.S), the per-secondary
// boot-stack guards (P5-secondary-stack-guard), and the current thread's
// per-thread kstack guard (P3-Bca; lower THREAD_KSTACK_GUARD_SIZE mapped
// no-access in the kernel direct map). F4 (SMP soundness sweep, 2026-05-31):
// the message NAMES the flavor so a fault is never misattributed -- a
// secondary's (or current-thread's) wild-SP fault must not read as a
// "_boot_stack" overflow, which is exactly what left the F-B early-boot
// overflow ambiguous (the boot CPU provably cannot deepen its stack in that
// window, so a _boot_stack-named fault there had no constructible cause).
static const char *stack_guard_overflow_msg(u64 addr) {
    // Boot stack guard (PA range, then high-VA range).
    if (addr >= sym_to_pa(_boot_stack_guard) && addr < sym_to_pa(_boot_stack_bottom))
        return "kernel stack overflow (boot-stack guard)";
    if (addr >= (u64)(uintptr_t)_boot_stack_guard &&
        addr < (u64)(uintptr_t)_boot_stack_bottom)
        return "kernel stack overflow (boot-stack guard)";

    // Per-secondary boot-stack guard pages (high-VA, then PA). A secondary's
    // idle thread runs on its boot stack (it owns no per-thread kstack), so an
    // overflow OR a wild SP there lands in that slot's guard page.
    for (unsigned c = 0; c < DTB_MAX_CPUS - 1; c++) {
        u64 sg_va = (u64)(uintptr_t)&g_secondary_boot_stacks[c].guard[0];
        if (addr >= sg_va && addr < sg_va + SECONDARY_STACK_GUARD_SIZE)
            return "kernel stack overflow (secondary-stack guard)";
        u64 sg_pa = sym_to_pa(&g_secondary_boot_stacks[c].guard[0]);
        if (addr >= sg_pa && addr < sg_pa + SECONDARY_STACK_GUARD_SIZE)
            return "kernel stack overflow (secondary-stack guard)";
    }

    // #867: cpu0's idle thread (bootcpu_idle) runs on g_bootcpu_idle_stack (a
    // struct secondary_stack); an overflow or a wild SP lands in its leading
    // guard page, mapped no-access by build_page_tables (high-VA, then PA).
    {
        u64 ig_va = (u64)(uintptr_t)&g_bootcpu_idle_stack.guard[0];
        if (addr >= ig_va && addr < ig_va + SECONDARY_STACK_GUARD_SIZE)
            return "kernel stack overflow (bootcpu-idle guard)";
        u64 ig_pa = sym_to_pa(&g_bootcpu_idle_stack.guard[0]);
        if (addr >= ig_pa && addr < ig_pa + SECONDARY_STACK_GUARD_SIZE)
            return "kernel stack overflow (bootcpu-idle guard)";
    }

    // Current thread's per-thread kstack guard (direct-map KVA).
    struct Thread *t = current_thread();
    if (t && t->magic == THREAD_MAGIC && t->kstack_base) {
        u64 b = (u64)(uintptr_t)t->kstack_base;
        if (addr >= b && addr < b + THREAD_KSTACK_GUARD_SIZE)
            return "kernel stack overflow (current-thread kstack guard)";
    }
    return NULL;
}

// True iff `addr` falls inside the kernel image's permanent mappings
// (TEXT / RODATA / DATA / BSS). Used to recognize W^X violations.
//
// Three address forms hit the kernel image:
//   1. PA range — left over from any pre-MMU access (rare).
//   2. Kernel-image high VA — kaslr_kernel_high_base() + offset (TTBR1).
//   3. Direct-map alias — pa_to_kva(kernel_image_pa) (P3-Bca).
static bool addr_is_kernel_image(u64 addr) {
    u64 ks_pa = kaslr_kernel_pa_start();
    u64 ke_pa = kaslr_kernel_pa_end();
    if (addr >= ks_pa && addr < ke_pa) return true;

    u64 ks_va = (u64)(uintptr_t)_kernel_start;
    u64 ke_va = (u64)(uintptr_t)_kernel_end;
    if (addr >= ks_va && addr < ke_va) return true;

    u64 ks_kva = (u64)(uintptr_t)pa_to_kva(ks_pa);
    u64 ke_kva = (u64)(uintptr_t)pa_to_kva(ke_pa);
    return addr >= ks_kva && addr < ke_kva;
}

// =============================================================================
// arch_fault_handle — top-level dispatcher.
// =============================================================================

enum fault_result arch_fault_handle(const struct fault_info *fi) {
    // 1. Stack-guard recognition (kernel-mode only — user-mode faults
    //    don't have access to kernel stack pages anyway).
    //
    //    Per ARCH §28: kernel stack overflow is detected by mapping the
    //    lower N pages of every kstack as no-access (P3-Bca direct-map
    //    L3 entries). An overflow access lands in one of those pages
    //    and faults; FAR_EL1 is in the guard region.
    if (!fi->from_user) {
        // #806 re-entrancy guard. Every kernel-fault branch below extincts
        // (a kernel fault is fatal at v1.0). But the handler itself can fault:
        // stack_guard_overflow_msg dereferences current_thread() (TPIDR_EL1),
        // so a wild current_thread -- e.g. a context switch to a corrupted
        // Thread -- makes t->magic fault, re-entering here. Unguarded, that
        // recurses one KERNEL_ENTRY frame (288 B) per fault until the boot
        // stack crosses its guard, and the REAL bug masquerades as a
        // "boot-stack guard" overflow (the F-B/#806 saga: the apparent depth
        // is a recursion amplifier, not honest call depth -- normal boot-stack
        // high-water is ~4.5 KiB of 16 KiB). On re-entry, extinct NOW with the
        // nested FAR (the wild address), before re-running the faulting code.
        // Never cleared: a kernel fault is always fatal, so the CPU is dying
        // either way; the flag only has to survive long enough to break the
        // recursion. (The HALLS dump's own re-entrancy guard, HX-I1, catches a
        // further fault inside the dump.)
        // Clamp a wild MPIDR onto slot 0 (mirrors halls_cpu) rather than
        // skipping -- so the guard stays LIVE under a corrupt Aff0 instead
        // of silently disabling itself in exactly the scenario it exists for.
        unsigned cpu = smp_cpu_idx_self();
        if (cpu >= DTB_MAX_CPUS) cpu = 0;
        if (g_in_kernel_fault[cpu]) {
            extinction_with_addr("recursive kernel fault (handler re-entered)",
                                 (uintptr_t)fi->vaddr);
        }
        g_in_kernel_fault[cpu] = true;

        // F4: name the guard flavor (boot / secondary / current-thread kstack)
        // so the fault is never misattributed. FAR_EL1 (fi->vaddr) shows where
        // the access landed; the flavor names which stack's guard it is. NB:
        // a "boot-stack guard" hit is NOT proof of honest boot-CPU depth -- a
        // recursive handler fault (above) descends the boot stack too; #806's
        // re-entrancy guard fires first so that case extincts honestly.
        const char *ovf = stack_guard_overflow_msg(fi->vaddr);
        if (ovf) extinction_with_addr(ovf, (uintptr_t)fi->vaddr);
    }

    // 2. W^X violation on the kernel image (kernel-mode only).
    //
    //    Per ARCH §28 I-12: every page is writable XOR executable. The
    //    PTE_KERN_TEXT bits enforce this at PTE construction; the MMU
    //    raises a permission fault on violation. Recognized when FSC
    //    indicates permission AND FAR is in the kernel image.
    if (!fi->from_user && fi->is_permission && addr_is_kernel_image(fi->vaddr)) {
        extinction_with_addr("PTE violates W^X (kernel image)",
                             (uintptr_t)fi->vaddr);
    }

    // 3. Translation fault (kernel-mode).
    //
    //    Kernel code touched a non-mapped VA. At v1.0 this is always a
    //    bug — the kernel maps every region it uses (direct map, vmalloc,
    //    kernel image). After P3-Bda retired TTBR0 identity, a stray
    //    PA-as-VA-via-TTBR0 access faults here. Extincts with the FAR
    //    so the operator can identify the address.
    if (!fi->from_user && fi->is_translation) {
        extinction_with_addr("unhandled kernel translation fault",
                             (uintptr_t)fi->vaddr);
    }

    // 4. Permission fault (kernel-mode, not in kernel image).
    //
    //    A permission fault outside the kernel image — e.g., kernel code
    //    wrote to a page mapped RO that isn't kernel image. Currently
    //    only direct-map RO regions could trigger this (P3-Bb-hardening:
    //    direct-map alias of .text is RO+XN). Extincts.
    if (!fi->from_user && fi->is_permission) {
        extinction_with_addr("unhandled kernel permission fault",
                             (uintptr_t)fi->vaddr);
    }

    // 5. Access-flag fault (kernel-mode).
    //
    //    FEAT_HAFDBS hardware access flag. At v1.0 PTE_AF is set eagerly
    //    by the PTE constructors so this shouldn't fire. If it does,
    //    something built a PTE without PTE_AF — bug.
    if (!fi->from_user && fi->is_access_flag) {
        extinction_with_addr("unhandled kernel access-flag fault",
                             (uintptr_t)fi->vaddr);
    }

    // 6. User-mode fault — demand paging via the VMA tree (P3-Dc).
    //
    //    Routes through `userland_demand_page` which performs vma_lookup,
    //    permission check, BURROW resolution, and PTE install. Returns
    //    FAULT_HANDLED on success → ERET resumes. Returns
    //    FAULT_UNHANDLED_USER if no VMA covers vaddr / permission denied
    //    / sub-table OOM — caller (exception.c) terminates the Proc via
    //    proc_fault_terminate + a snare:* note (ERRORS.md; the v1.0
    //    extinction policy was retired by P6 hardening #3a).
    //
    //    This is the live production demand-page path (every EL0 first
    //    touch lands here); tests additionally drive `userland_demand_page`
    //    directly with a synthetic fault_info + manually-constructed Proc.
    if (fi->from_user) {
        struct Thread *t = current_thread();
        if (!t || t->magic != THREAD_MAGIC) return FAULT_UNHANDLED_USER;
        if (!t->proc || t->proc->magic != PROC_MAGIC) return FAULT_UNHANDLED_USER;
        return userland_demand_page(t->proc, fi);
    }

    // 7. Catch-all.
    extinction_with_addr("unclassified kernel fault (ESR)",
                         (uintptr_t)fi->esr);
}

// =============================================================================
// P3-Dc: userland_demand_page — VMA → BURROW → PTE install pipeline.
// =============================================================================

// REVENANT R-2: the FILE-fault "needs a page-in" request. demand_page_locked
// fills this (under vma_lock) on a BURROW_TYPE_FILE miss -- the page is not yet
// resident -- so userland_demand_page can do the BLOCKING dev->read OUTSIDE
// vma_lock (a spinlock cannot be held across a 9P round-trip), then re-acquire
// + re-validate + install-once. `burrow` is PINNED via burrow_ref here so it
// (and its SLUB address) cannot be freed/reused while we sleep on the read --
// the slow path drops that ref OUTSIDE vma_lock (the last unref's spoor_clunk
// may sleep). This makes the post-read re-validation UAF- and ABA-safe.
struct file_fault_req {
    bool           needed;       // a FILE miss occurred -> run the slow path
    struct Burrow *burrow;       // the FILE Burrow, pinned (burrow_ref) across the read
    struct Spoor  *spoor;        // the pinned backing Chan to dev->read
    u64            file_offset;  // byte offset in the backing file for this page
    u64            page_va;      // page-aligned fault VA (the PTE target)
    size_t         slot;         // page index within burrow->filepages
    bool           exec;         // mapping is executable -> I-cache sync the page-in
};

// Locked core. Caller validated p and holds p->vma_lock across the whole
// lookup -> burrow-resolve -> PTE-install sequence (see userland_demand_page).
// On a BURROW_TYPE_FILE miss the page is not resident: this fills *freq (a
// caller-zeroed request), pins the Burrow, and returns -- userland_demand_page
// then runs the slow path (read OUTSIDE the lock). freq->needed disambiguates a
// FILE-miss (run the slow path; the returned value is ignored) from a genuine
// failure (FAULT_UNHANDLED_USER) or a non-FILE / FILE-resident-hit fast install
// (FAULT_HANDLED).
static enum fault_result demand_page_locked(struct Proc *p,
                                            const struct fault_info *fi,
                                            struct file_fault_req *freq) {
    // 1. VMA lookup.
    struct Vma *vma = vma_lookup(p, fi->vaddr);
    if (!vma)                            return FAULT_UNHANDLED_USER;

    // 2. Permission check vs fault type.
    //    - Write fault → VMA must permit WRITE.
    //    - Instruction fault → VMA must permit EXEC.
    //    - Read fault → VMA must permit READ (any of R / RW / RX).
    if (fi->is_write && !(vma->prot & VMA_PROT_WRITE)) {
        return FAULT_UNHANDLED_USER;
    }
    if (fi->is_instruction && !(vma->prot & VMA_PROT_EXEC)) {
        return FAULT_UNHANDLED_USER;
    }
    if (!fi->is_write && !fi->is_instruction &&
        !(vma->prot & VMA_PROT_READ)) {
        return FAULT_UNHANDLED_USER;
    }

    // 3. Resolve the BURROW offset for the page covering fi->vaddr.
    u64 page_va        = fi->vaddr & ~(PAGE_SIZE - 1);
    u64 in_vma_offset  = page_va - vma->vaddr_start;
    u64 burrow_byte_off   = vma->burrow_offset + in_vma_offset;

    if (!vma->burrow)                       return FAULT_UNHANDLED_USER;
    if (vma->burrow->magic != VMO_MAGIC)    return FAULT_UNHANDLED_USER;
    if (burrow_byte_off >= vma->burrow->size)  return FAULT_UNHANDLED_USER;

    // 4. Resolve to a backing PA. P4-Ic2 + P4-Ic5b1b: dispatch on burrow type.
    //    - BURROW_TYPE_ANON: pages is the head of a contiguous alloc_pages
    //      chunk; page i is at page_to_pa(pages) + i * PAGE_SIZE.
    //      PTE attrs are MAIR_IDX_NORMAL_WB (cacheable RAM).
    //    - BURROW_TYPE_MMIO: pa is the device PA (page-aligned, fixed);
    //      page i is at burrow->pa + i * PAGE_SIZE.
    //      PTE attrs are MAIR_IDX_DEVICE (nGnRnE).
    //    - BURROW_TYPE_DMA (P4-Ic5b1b): pa is the buddy-chosen PA of the
    //      pinned page chunk owned by the underlying KObj_DMA; page i is
    //      at burrow->pa + i * PAGE_SIZE. PTE attrs are
    //      MAIR_IDX_NORMAL_WB (cacheable RAM) — DMA buffers are
    //      coherent on QEMU virt's VirtIO transports, so the CPU writes
    //      with normal cache attributes and the device sees the same
    //      bytes via snoop. Hardware that requires uncached DMA buffers
    //      (Phase 5+ real platforms) will introduce a flag at create
    //      time to select Device attrs.
    paddr_t page_pa;
    bool    device_memory;
    switch (vma->burrow->type) {
    case BURROW_TYPE_ANON:
        if (!vma->burrow->pages)            return FAULT_UNHANDLED_USER;
        page_pa = page_to_pa(vma->burrow->pages) +
                  (burrow_byte_off & ~(u64)(PAGE_SIZE - 1));
        device_memory = false;
        break;
    case BURROW_TYPE_MMIO:
        if (!vma->burrow->kobj_mmio)        return FAULT_UNHANDLED_USER;
        page_pa = vma->burrow->pa +
                  (burrow_byte_off & ~(u64)(PAGE_SIZE - 1));
        device_memory = true;
        break;
    case BURROW_TYPE_DMA:
        if (!vma->burrow->kobj_dma)         return FAULT_UNHANDLED_USER;
        page_pa = vma->burrow->pa +
                  (burrow_byte_off & ~(u64)(PAGE_SIZE - 1));
        device_memory = false;
        break;
    case BURROW_TYPE_FILE: {
        // REVENANT R-2 / I-36: the page comes from the sparse filepages[] array,
        // demand-read from the pinned backing Spoor. Slot index within the
        // segment (burrow_byte_off < size was checked above, so slot is in range).
        struct Burrow *v = vma->burrow;
        size_t slot = (burrow_byte_off & ~(u64)(PAGE_SIZE - 1)) / PAGE_SIZE;
        spin_lock(&v->lock);            // vma_lock -> v->lock (the established order)
        struct page *resident =
            (v->filepages && slot < v->page_count) ? v->filepages[slot] : NULL;
        spin_unlock(&v->lock);
        if (resident) {
            // Fast path: resident hit. Each FILE slot is its own order-0 page,
            // so page_pa is the slot page's PA (no contiguous-chunk offset).
            page_pa = page_to_pa(resident);
            device_memory = false;
            break;                      // -> step-5 PTE install (R+X for text)
        }
        // Miss: pin the Burrow across the lockless read (so freq->burrow cannot
        // be freed/ABA'd while we sleep -- the VMA holds a mapping ref so the
        // Burrow is alive NOW, making burrow_ref safe). file_demand_page_slow
        // drops this ref OUTSIDE vma_lock. Lock order vma_lock -> v->lock
        // (burrow_ref takes v->lock internally).
        burrow_ref(v);
        freq->needed      = true;
        freq->burrow      = v;
        freq->spoor       = v->spoor;
        freq->file_offset = v->file_offset + (burrow_byte_off & ~(u64)(PAGE_SIZE - 1));
        freq->page_va     = page_va;
        freq->slot        = slot;
        freq->exec        = (vma->prot & VMA_PROT_EXEC) != 0;  // text -> I-cache sync
        return FAULT_UNHANDLED_USER;    // ignored by the caller (freq->needed set)
    }
    case BURROW_TYPE_ANON_LAZY: {
        // Overcommit / I-32 (ARCH §6.5; SYS_BURROW_ATTACH_LAZY): demand-ZERO. The
        // page is allocated, zero-filled, and installed RW/XN on first touch. The
        // structural twin of the FILE arm but SIMPLER -- no backing read, so the
        // whole fill runs under the already-held vma_lock (alloc_pages under vma_lock
        // is the established order: SYS_BURROW_ATTACH holds vma_lock across
        // burrow_create_anon -> alloc_pages). No slow path, no pin, no
        // death-interruptible read.
        struct Burrow *v = vma->burrow;
        size_t slot = (burrow_byte_off & ~(u64)(PAGE_SIZE - 1)) / PAGE_SIZE;

        spin_lock(&v->lock);            // vma_lock -> v->lock (the established order)
        struct page *resident =
            (v->filepages && slot < v->page_count) ? v->filepages[slot] : NULL;
        spin_unlock(&v->lock);
        if (resident) {
            // Resident hit (a re-fault, or a sibling faulter filled it): the page was
            // charged when it was allocated -- do NOT charge again.
            page_pa = page_to_pa(resident);
            device_memory = false;
            break;                      // -> step-5 PTE install
        }

        // Miss: charge the I-32 page_count BEFORE the alloc so page_count == true RSS
        // and a cap-hit frees nothing. An over-PROC_PAGE_MAX commit on a non-TCB Proc
        // fails the fault here -> the caller proc_fault_terminates (graceful OOM,
        // never a box extinction -- I-32; the same backstop the eager attach gives at
        // attach time, moved to fault time for the free reservation).
        if (!proc_page_charge(p, 1))
            return FAULT_UNHANDLED_USER;
        struct page *newpg = alloc_pages(0, KP_ZERO);
        if (!newpg) {
            proc_page_uncharge(p, 1);
            return FAULT_UNHANDLED_USER;    // OOM -> graceful per-Proc terminate
        }

        // Install-once into the slot. Under vma_lock (held by the caller across the
        // whole demand_page_locked) no sibling faulter of this Proc can touch
        // filepages -- they serialize on vma_lock -- so at v1.0 (one mapping, one
        // Proc) the slot is still NULL here. The v->lock re-check + loser-free is the
        // audited FILE-arm install-once shape, defensive against a future shared-lazy
        // mapping; the loser branch is unreachable at v1.0.
        struct page *winner;
        struct page *loser = NULL;
        spin_lock(&v->lock);
        if (!v->filepages || slot >= v->page_count) {
            spin_unlock(&v->lock);
            free_pages(newpg, 0);
            proc_page_uncharge(p, 1);
            return FAULT_UNHANDLED_USER;    // impossible shape change; bail safe
        }
        if (v->filepages[slot]) {
            winner = v->filepages[slot];    // a sibling won the race
            loser  = newpg;
        } else {
            v->filepages[slot] = newpg;     // we win -- the Burrow owns newpg now
            winner = newpg;
        }
        spin_unlock(&v->lock);
        if (loser) {
            free_pages(loser, 0);           // free OUTSIDE v->lock (leaf order)
            proc_page_uncharge(p, 1);       // we double-charged; give it back
        }
        page_pa = page_to_pa(winner);
        device_memory = false;
        break;                              // -> step-5 PTE install (RW/XN, W^X-clean)
    }
    case BURROW_TYPE_INVALID:
    default:
        return FAULT_UNHANDLED_USER;
    }

    // 5. Install the leaf PTE in the per-Proc TTBR0 tree. The asid arg is
    // vestigial (the install does an all-ASID `tlbi vaae1is`); pass 0 -- the
    // Proc's rolling ASID is resolved at context switch, not here (RW-1 B-F1).
    int rc = mmu_install_user_pte(p->pgtable_root, 0,
                                  page_va, page_pa, vma->prot,
                                  device_memory);
    if (rc != 0)                         return FAULT_UNHANDLED_USER;

    return FAULT_HANDLED;
}

// P3-Dc: userland_demand_page — VMA → BURROW → PTE install pipeline.
//
// P6 #713 root-cause fix (multi-thread-Proc SMP safety): serialize the whole
// lookup -> burrow-resolve -> PTE-install under p->vma_lock. This completes
// the vma_lock coverage the proc.h comment explicitly defers to "the
// pouch-threads sub-chunk ... the page-fault vma_lookup reader" -- which the
// threads lift never wired up. Pre-fix, the unlocked reader raced a sibling
// thread's SYS_BURROW_DETACH (vma_remove + free_pages, under vma_lock): the
// walker could follow a half-unlinked list to a freed VMA and install a leaf
// PTE aliasing a buddy page already recycled into kernel (or another Proc's)
// memory -> a wild/kernel pointer in the faulting Proc's address space (the
// observed 0xffff.. fault). The same lock also serializes two concurrent
// faults that would otherwise race the intermediate page-table construction
// in mmu_install_user_pte (orphaning a sub-table). Uncontended for
// single-thread Procs (the v1.0 common case). Lock order vma_lock ->
// buddy_lock matches SYS_BURROW_ATTACH (vma_lock held across
// burrow_create_anon -> alloc_pages), so no inversion.
// REVENANT R-2 / I-36: file_install_locked -- the FILE slow-path tail, run
// under the RE-ACQUIRED p->vma_lock. The Burrow is pinned (freq->burrow's
// burrow_ref, taken in demand_page_locked), so it cannot have been freed (nor
// its SLUB address reused) while we slept on the read; but the VMA itself may
// have been torn down (a sibling thread's SYS_BURROW_DETACH) or replaced, so
// re-lookup + re-validate it still maps the SAME pinned FILE Burrow. Then
// install-once under v->lock (a sibling faulter may have filled the slot during
// our read -- benign double-read; the loser frees its page) and install the R+X
// PTE. Does NOT drop the Burrow ref (the caller does, OUTSIDE vma_lock). `newpg`
// is consumed on every path (stored into the slot, or freed).
static enum fault_result file_install_locked(struct Proc *p,
                                             const struct fault_info *fi,
                                             struct file_fault_req *freq,
                                             struct page *newpg) {
    struct Vma *vma = vma_lookup(p, fi->vaddr);
    if (!vma || vma->burrow != freq->burrow ||
        vma->burrow->magic != VMO_MAGIC ||
        vma->burrow->type != BURROW_TYPE_FILE) {
        // Raced a VMA teardown / remap while we read. The pinned Burrow is still
        // alive (our ref); it just no longer maps this VA. Drop our page; the
        // caller drops the pin. A re-fault re-resolves against the new state.
        free_pages(newpg, 0);
        return FAULT_UNHANDLED_USER;
    }
    struct Burrow *v = vma->burrow;
    // freq->slot was computed pre-sleep from the original VMA's geometry; it stays
    // valid against this re-looked-up VMA because a BURROW_TYPE_FILE Burrow is
    // created only by exec, mapped exactly once at burrow_offset 0 (burrow_map's
    // fixed offset arg), and never handed to userspace to re-map -- so a FILE
    // Burrow has ONE fixed VMA over its life and freq->page_va -> freq->slot is
    // stable. (R-5 audit F2 [P3, unreachable at v1.0]: if a future path ever maps a
    // FILE Burrow at a chosen offset/VA, recompute slot here from
    // vma->burrow_offset + (freq->page_va - vma->vaddr_start) rather than trusting
    // the cached freq->slot.)

    struct page *resident;
    spin_lock(&v->lock);                 // vma_lock -> v->lock (established order)
    if (!v->filepages || freq->slot >= v->page_count) {
        spin_unlock(&v->lock);
        free_pages(newpg, 0);
        return FAULT_UNHANDLED_USER;     // impossible shape change; bail safe
    }
    if (v->filepages[freq->slot]) {
        resident = v->filepages[freq->slot];   // a sibling faulter won the race
    } else {
        v->filepages[freq->slot] = newpg;      // we win -- the Burrow owns newpg now
        resident = newpg;
    }
    spin_unlock(&v->lock);
    if (resident != newpg)
        free_pages(newpg, 0);            // discard the loser's freshly-read page

    // Install the leaf PTE at vma->prot (R+X for text, never writable -- W^X
    // I-12 holds by construction). Each FILE slot is its own order-0 page, so
    // the PA is the slot page's PA (no contiguous-chunk offset). The asid arg
    // is vestigial (all-ASID tlbi); pass 0 (RW-1 B-F1).
    int rc = mmu_install_user_pte(p->pgtable_root, 0, freq->page_va,
                                  page_to_pa(resident), vma->prot, /*device=*/false);
    if (rc != 0) return FAULT_UNHANDLED_USER;
    return FAULT_HANDLED;
}

// REVENANT R-2 / I-36: file_demand_page_single -- the proven single-page
// BURROW_TYPE_FILE fill, run with NO lock held (vma_lock was released by
// userland_demand_page after demand_page_locked signalled the miss). Allocates a
// fresh zeroed page, reads ONE page from the pinned backing Spoor (BLOCKS on the
// 9P round-trip; #811-death-interruptible BY INHERITANCE from the dev9p read -- a
// dying Proc unwinds at el0_return_die_check and the read returns < 0; no new
// wait/wake code), then re-acquires vma_lock to install. Fail-closed: a read
// error -> FAULT_USER_BUS (snare:bus, I-36 condition 6), NEVER a silent zero-fill
// of executable text. This is ALSO the read-ahead FALLBACK -- file_demand_page_
// cluster degrades here on any staging/page alloc shortfall or a degenerate
// 1-page cluster. Does NOT drop the Burrow pin: the file_demand_page_slow entry
// drops it exactly once, OUTSIDE vma_lock, after the fill returns.
static enum fault_result file_demand_page_single(struct Proc *p,
                                               const struct fault_info *fi,
                                               struct file_fault_req *freq) {
    enum fault_result r;

    struct page *newpg = alloc_pages(0, KP_ZERO);
    if (!newpg) {
        r = FAULT_UNHANDLED_USER;        // OOM -> graceful per-Proc terminate
        goto out;
    }

    // Fill the page from the backing file. dev->read may legitimately short-return
    // for an interior page (a conforming 9P server's choice -- dev9p_read issues
    // ONE p9_client_read = ONE Tread and returns its count), so a SINGLE read could
    // leave the page tail KP_ZERO with file bytes MISSING -> a corrupt interior
    // text page. Loop until the page is full OR n == 0 (true EOF: the file's final
    // partial page, where the tail legitimately stays zero = the .bss-style fill).
    // This mirrors map_eager_from_file + exec_read_header, which loop for the same
    // reason. (R-5 audit SA-F1: the single-read version corrupted a Stratum-backed
    // binary's interior text on a partial Rread; devramfs reads full-count, so the
    // boot's /bin-bound binaries never tripped it.)
    void *kva = pa_to_kva(page_to_pa(newpg));
    if (!freq->spoor || !freq->spoor->dev || !freq->spoor->dev->read) {
        free_pages(newpg, 0);
        r = FAULT_USER_BUS;              // no backing read -> fail closed, never zero-fill
        goto out;
    }
    size_t got = 0;
    while (got < PAGE_SIZE) {
        long n = freq->spoor->dev->read(freq->spoor, (u8 *)kva + got,
                                        (long)(PAGE_SIZE - got),
                                        (s64)(freq->file_offset + got));
        if (n < 0) {
            free_pages(newpg, 0);
            r = FAULT_USER_BUS;          // page-in I/O error / death-interrupt -> snare:bus
            goto out;
        }
        if (n == 0) break;               // EOF -> the tail stays zero (KP_ZERO)
        got += (size_t)n;
    }

    // REVENANT R-4 / I-36: the page was filled via the data path (dev->read);
    // an executable mapping (text) fetches it via the I-cache, which is not
    // coherent with the D-cache for instruction fetch on ARMv8. Clean to PoU +
    // invalidate the I-cache BEFORE the PTE is installed (the install below is
    // EL0's only reach to the page), or EL0 could fetch a stale line from a
    // prior occupant of this recycled PA -> wrong-instruction execution. The
    // race-loser's page (a sibling faulter won the slot) is discarded in
    // file_install_locked; whichever page backs the PTE was synced by ITS
    // creator, so every installed text page is coherent. Non-exec FILE pages
    // (R-only rodata since #45) skip the sync -- sound because each FILE
    // Burrow backs ONE segment at ONE prot, so no page ever migrates from a
    // non-exec to an exec mapping (REVENANT 4.6).
    if (freq->exec)
        arch_icache_sync_range(kva, PAGE_SIZE);

    spin_lock(&p->vma_lock);
    r = file_install_locked(p, fi, freq, newpg);
    spin_unlock(&p->vma_lock);

out:
    return r;
}

// REVENANT read-ahead (#343). A FILE page-in demand-reads 4 KiB per fault, and
// each 4 KiB 9P Tread lands in an 8 MiB Stratum extent that Stratum decrypts +
// Merkle-verifies WHOLE (the ~1000x amplification documented in stratum-fs
// run.c::cmd_read). Paging the toolchain (compile/asm/link ~28 MiB) that way is
// thousands of round-trips x per-extent decrypts -- ~400x slower than the host.
// Read-ahead batches the READ across a page cluster into one dev->read (one 9P
// round-trip, and -- with a window a good fraction of the extent -- far fewer
// decrypts), while the cheap PTE install stays per-fault: a cluster-mate's later
// touch hits the resident fast path (demand_page_locked FILE resident-hit) and
// installs its own PTE with no read. The Linux page-cache readahead model.
//
// filepages[] semantics are UNCHANGED (each slot an independent order-0 page), so
// the free arm / decommit / install-once / resident-check are untouched -- the
// cluster path only batches the FILL. It is byte-identical to N sequential
// single-page reads (same offsets, same EOF-tail-stays-zero), preserves W^X (R+X
// install via vma->prot), I-cache-syncs each text page BEFORE any PTE backs it,
// is death-interruptible + fail-closed (a read error/death -> snare:bus, never a
// zero-filled text page), and is BEST-EFFORT: any alloc shortfall or a degenerate
// 1-page cluster degrades to file_demand_page_single -- read-ahead never fails a
// fault. The Burrow pin is held across the whole batched read (freq->burrow), and
// dropped once by the file_demand_page_slow entry.
#define REVENANT_READAHEAD_PAGES 64u    // 256 KiB cluster (vs a 4 KiB single page)

// Copy one page from the (page-aligned) staging buffer into a (page-aligned)
// destination page as u64 words -- the freestanding kernel has no memcpy symbol,
// and both operands are page-base-aligned (8-byte aligned), so the word copy is
// safe. PAGE_SIZE (4 KiB) / 8 = 512 stores.
static inline void revenant_copy_page(void *dst, const void *src) {
    u64 *d = (u64 *)dst;
    const u64 *s = (const u64 *)src;
    for (size_t w = 0; w < PAGE_SIZE / sizeof(u64); w++)
        d[w] = s[w];
}

// Install a filled cluster under the RE-ACQUIRED vma_lock: re-validate the VMA
// still maps the SAME pinned FILE Burrow, install-once each cluster slot into
// filepages[] (a sibling faulter may have won a slot -- benign; the loser page is
// freed), then install ONLY the faulting slot's PTE (cluster-mates install on
// their own resident-hit faults). clpages[i] is NULL'd when the Burrow adopts it
// (winner) and left set when WE still own it (loser, or an early bail) so the
// caller frees exactly the un-adopted pages, OUTSIDE v->lock (mirrors the
// single-page loser-free discipline). Consumes every clpages[] entry on every
// path (adopted into a slot, or freed by the caller).
//
// The pre-sleep [cstart, cstart+ncluster) slot RANGE (and the file bytes read
// for it) stays valid against the re-looked-up VMA for the same reason the
// single path's cached freq->slot does (the R-5 F2 justification at
// file_install_locked): a FILE Burrow is created only by exec and mapped
// exactly once at burrow_offset 0, never handed to userspace to re-map, and
// its geometry scalars (page_count / file_offset / size) are immutable
// post-create. The range install leans HARDER on that invariant than the
// single slot did -- a future chosen-offset FILE map must recompute cstart
// from vma->burrow_offset here, not just freq->slot.
static enum fault_result file_install_cluster_locked(struct Proc *p,
                                                     const struct fault_info *fi,
                                                     struct file_fault_req *freq,
                                                     size_t cstart, size_t ncluster,
                                                     struct page **clpages) {
    struct Vma *vma = vma_lookup(p, fi->vaddr);
    if (!vma || vma->burrow != freq->burrow ||
        vma->burrow->magic != VMO_MAGIC ||
        vma->burrow->type != BURROW_TYPE_FILE) {
        return FAULT_UNHANDLED_USER;     // raced teardown/remap; caller frees clpages
    }
    struct Burrow *v = vma->burrow;

    struct page *fault_pg = NULL;
    spin_lock(&v->lock);                 // vma_lock -> v->lock (established order)
    if (!v->filepages || cstart + ncluster > v->page_count) {
        spin_unlock(&v->lock);
        return FAULT_UNHANDLED_USER;     // impossible shape change; caller frees clpages
    }
    for (size_t i = 0; i < ncluster; i++) {
        size_t s = cstart + i;
        struct page *winner;
        if (v->filepages[s]) {
            winner = v->filepages[s];        // sibling won; clpages[i] stays a loser
        } else {
            v->filepages[s] = clpages[i];    // we win -- the Burrow owns it now
            winner = clpages[i];
            clpages[i] = NULL;               // adopted: caller must not free it
        }
        if (s == freq->slot) fault_pg = winner;
    }
    spin_unlock(&v->lock);

    // freq->slot is in [cstart, cstart+ncluster) by construction, so fault_pg is
    // always set; defensive bail keeps the invariant local.
    if (!fault_pg) return FAULT_UNHANDLED_USER;

    // Install the leaf PTE at vma->prot (R+X for text, never writable -- W^X /
    // I-12). The asid arg is vestigial (all-ASID tlbi); pass 0.
    int rc = mmu_install_user_pte(p->pgtable_root, 0, freq->page_va,
                                  page_to_pa(fault_pg), vma->prot, /*device=*/false);
    if (rc != 0) return FAULT_UNHANDLED_USER;
    return FAULT_HANDLED;
}

// REVENANT read-ahead: the cluster FILL (NO lock held). Batches ONE dev->read of
// up to REVENANT_READAHEAD_PAGES pages into filepages[] around the faulting slot.
// Degrades to file_demand_page_single on any alloc shortfall / degenerate cluster
// so a fault always makes progress. Does NOT drop the Burrow pin (the entry does).
static enum fault_result file_demand_page_cluster(struct Proc *p,
                                                  const struct fault_info *fi,
                                                  struct file_fault_req *freq) {
    struct Burrow *v = freq->burrow;     // pinned; geometry scalars are immutable
                                         // post-create (size/page_count/file_offset).
    // Cluster [cstart, cend) of slots, aligned down from the faulting slot and
    // clamped to page_count. ncluster >= 1 (freq->slot is in range).
    size_t cstart = freq->slot & ~(size_t)(REVENANT_READAHEAD_PAGES - 1u);
    size_t cend   = cstart + REVENANT_READAHEAD_PAGES;
    if (cend > v->page_count) cend = v->page_count;
    size_t ncluster = cend - cstart;
    if (ncluster <= 1)
        return file_demand_page_single(p, fi, freq);

    if (!freq->spoor || !freq->spoor->dev || !freq->spoor->dev->read)
        return FAULT_USER_BUS;           // no backing read -> fail closed (never zero-fill)

    // Staging buffer for ONE batched read of the whole cluster. KP_ZERO so a short
    // final read (EOF) leaves the tail zero (.bss-style fill), matching the single
    // path. order = smallest power-of-two >= ncluster. alloc failure -> degrade.
    unsigned sorder = 0;
    while ((size_t)(1u << sorder) < ncluster) sorder++;
    struct page *stage = alloc_pages(sorder, KP_ZERO);
    if (!stage)
        return file_demand_page_single(p, fi, freq);
    u8 *sbuf = (u8 *)pa_to_kva(page_to_pa(stage));

    u64    cl_file_off = v->file_offset + (u64)cstart * PAGE_SIZE;
    size_t cl_bytes    = ncluster * PAGE_SIZE;

    // Read the cluster: looped on short reads, death-interruptible + fail-closed,
    // exactly as the single-page path. Byte-identical to ncluster sequential
    // single-page reads (same offsets; the trailing EOF region stays KP_ZERO).
    size_t got = 0;
    while (got < cl_bytes) {
        long n = freq->spoor->dev->read(freq->spoor, sbuf + got,
                                        (long)(cl_bytes - got),
                                        (s64)(cl_file_off + got));
        if (n < 0) {
            free_pages(stage, sorder);
            return FAULT_USER_BUS;
        }
        if (n == 0) break;               // EOF -> remaining tail stays zero
        got += (size_t)n;
    }

    // One independent order-0 page per slot, filled from the staging buffer, with
    // the I-cache sync per text page BEFORE any PTE can back it. On ANY page-alloc
    // shortfall: free what we have + the staging, and degrade to single-page for
    // the faulting slot. (REVENANT_READAHEAD_PAGES pointers = 0.5 KiB of kstack.)
    struct page *clpages[REVENANT_READAHEAD_PAGES];
    for (size_t i = 0; i < ncluster; i++) clpages[i] = NULL;
    bool alloc_ok = true;
    for (size_t i = 0; i < ncluster; i++) {
        struct page *pg = alloc_pages(0, 0);    // fully memcpy'd below; no KP_ZERO
        if (!pg) { alloc_ok = false; break; }
        void *pkva = pa_to_kva(page_to_pa(pg));
        revenant_copy_page(pkva, sbuf + i * PAGE_SIZE);
        if (freq->exec)
            arch_icache_sync_range(pkva, PAGE_SIZE);
        clpages[i] = pg;
    }
    free_pages(stage, sorder);           // staging consumed (memcpy'd out)
    if (!alloc_ok) {
        for (size_t i = 0; i < ncluster; i++)
            if (clpages[i]) free_pages(clpages[i], 0);
        return file_demand_page_single(p, fi, freq);
    }

    // Install under the re-acquired vma_lock; free any un-adopted (loser/bail)
    // pages OUTSIDE v->lock (file_install_cluster_locked NULL'd the adopted ones).
    spin_lock(&p->vma_lock);
    enum fault_result r =
        file_install_cluster_locked(p, fi, freq, cstart, ncluster, clpages);
    spin_unlock(&p->vma_lock);
    for (size_t i = 0; i < ncluster; i++)
        if (clpages[i]) free_pages(clpages[i], 0);
    return r;
}

// REVENANT R-2 / I-36: the BURROW_TYPE_FILE slow-path ENTRY. Runs the read-ahead
// cluster fill (which degrades to the single-page path as needed), then drops the
// Burrow pin exactly once, OUTSIDE vma_lock (the last unref's burrow_free_internal
// -> spoor_clunk may sleep -- leaf-lock discipline).
static enum fault_result file_demand_page_slow(struct Proc *p,
                                               const struct fault_info *fi,
                                               struct file_fault_req *freq) {
    enum fault_result r = file_demand_page_cluster(p, fi, freq);
    burrow_unref(freq->burrow);
    return r;
}

enum fault_result userland_demand_page(struct Proc *p,
                                       const struct fault_info *fi) {
    if (!p || !fi)                       return FAULT_UNHANDLED_USER;
    if (p->magic != PROC_MAGIC)          return FAULT_UNHANDLED_USER;
    if (p->pgtable_root == 0)            return FAULT_UNHANDLED_USER;

    // Fast path under vma_lock: resolve + install (ANON/MMIO/DMA + a FILE
    // resident-hit). On a BURROW_TYPE_FILE miss, demand_page_locked sets
    // freq.needed + pins the Burrow; the slow path then runs OUTSIDE the lock
    // (the blocking dev->read cannot run under a spinlock -- the I-36 condition-5
    // death-interruptible page-in).
    struct file_fault_req freq = {0};
    spin_lock(&p->vma_lock);
    enum fault_result r = demand_page_locked(p, fi, &freq);
    spin_unlock(&p->vma_lock);

    if (freq.needed)
        return file_demand_page_slow(p, fi, &freq);
    return r;
}
