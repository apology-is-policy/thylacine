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

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

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
    //    / sub-table OOM — caller (exception.c) extincts at v1.0; Phase
    //    5+ note delivery upgrades this to SIGSEGV.
    //
    //    At v1.0 pre-P3-E this branch is dead code in production (no
    //    EL0 thread runs); tests drive `userland_demand_page` directly
    //    with a synthetic fault_info + manually-constructed Proc.
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

// Locked core. Caller validated p and holds p->vma_lock across the whole
// lookup -> burrow-resolve -> PTE-install sequence (see userland_demand_page).
static enum fault_result demand_page_locked(struct Proc *p,
                                            const struct fault_info *fi) {
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
enum fault_result userland_demand_page(struct Proc *p,
                                       const struct fault_info *fi) {
    if (!p || !fi)                       return FAULT_UNHANDLED_USER;
    if (p->magic != PROC_MAGIC)          return FAULT_UNHANDLED_USER;
    if (p->pgtable_root == 0)            return FAULT_UNHANDLED_USER;

    spin_lock(&p->vma_lock);
    enum fault_result r = demand_page_locked(p, fi);
    spin_unlock(&p->vma_lock);
    return r;
}
