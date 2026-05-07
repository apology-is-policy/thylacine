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
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

// Linker symbols — boot stack guard region + kernel image bounds.
extern char _boot_stack_guard[];
extern char _boot_stack_bottom[];
extern char _kernel_start[];
extern char _kernel_end[];

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

// True iff `addr` falls inside any active stack guard region — the boot
// stack guard (start.S) OR the per-thread kstack guard (P3-Bca; lower
// THREAD_KSTACK_GUARD_SIZE of each thread_create'd kstack mapped no-
// access in the kernel direct map). Both flavors produce the same
// "kernel stack overflow" extinction.
static bool addr_is_stack_guard(u64 addr) {
    // Boot stack guard via PA range.
    u64 guard_pa  = sym_to_pa(_boot_stack_guard);
    u64 bottom_pa = sym_to_pa(_boot_stack_bottom);
    if (addr >= guard_pa && addr < bottom_pa) return true;

    // Boot stack guard via high-VA range.
    u64 guard_va  = (u64)(uintptr_t)_boot_stack_guard;
    u64 bottom_va = (u64)(uintptr_t)_boot_stack_bottom;
    if (addr >= guard_va && addr < bottom_va) return true;

    // Per-thread kstack guard. P3-Bca: t->kstack_base is a direct-map KVA;
    // the lower THREAD_KSTACK_GUARD_SIZE bytes are mapped no-access via
    // mmu_set_no_access_range. Stack overflow lands inside that range,
    // and FAR_EL1 is the direct-map KVA of the guard page.
    struct Thread *t = current_thread();
    if (t && t->magic == THREAD_MAGIC && t->kstack_base) {
        u64 t_guard_base = (u64)(uintptr_t)t->kstack_base;
        u64 t_guard_end  = t_guard_base + THREAD_KSTACK_GUARD_SIZE;
        if (addr >= t_guard_base && addr < t_guard_end) return true;
    }

    return false;
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
    if (!fi->from_user && addr_is_stack_guard(fi->vaddr)) {
        extinction_with_addr("kernel stack overflow",
                             (uintptr_t)fi->vaddr);
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

enum fault_result userland_demand_page(struct Proc *p,
                                       const struct fault_info *fi) {
    if (!p || !fi)                       return FAULT_UNHANDLED_USER;
    if (p->magic != PROC_MAGIC)          return FAULT_UNHANDLED_USER;
    if (p->pgtable_root == 0)            return FAULT_UNHANDLED_USER;

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

    // 4. Resolve to a backing PA. v1.0 anonymous BURROW: burrow->pages is the
    //    head of a contiguous alloc_pages chunk; page i is at
    //    page_to_pa(burrow->pages) + i * PAGE_SIZE.
    if (!vma->burrow->pages)                return FAULT_UNHANDLED_USER;
    paddr_t burrow_base_pa = page_to_pa(vma->burrow->pages);
    paddr_t page_pa     = burrow_base_pa + (burrow_byte_off & ~(u64)(PAGE_SIZE - 1));

    // 5. Install the leaf PTE in the per-Proc TTBR0 tree.
    int rc = mmu_install_user_pte(p->pgtable_root, p->asid,
                                  page_va, page_pa, vma->prot);
    if (rc != 0)                         return FAULT_UNHANDLED_USER;

    return FAULT_HANDLED;
}
