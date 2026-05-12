// R12-uaccess: kernel-side user-VA accessor primitives.
//
// SYS_PUTS and similar syscall handlers need to dereference user VAs
// while executing in EL1. The user page may live in a VMA but not yet
// be installed in the per-Proc TTBR0 tree (lazy demand-paging) — the
// EL1 load then takes a translation fault. Pre-R12-uaccess, the kernel
// dispatcher treated kernel-mode translation faults as ELEs; userspace
// crates worked around it via `pretouch_rodata_pages()` that read each
// .rodata page from EL0 before calling SYS_PUTS, forcing demand-page
// installation via the EL0 fault path. That discipline is fragile
// (each binary has to know its own LOAD layout) and doesn't cover
// stack/heap/anon VAs.
//
// The proper fix is fault-fixup machinery: each kernel-mode user-VA
// load is a designated "uaccess" instruction. The fault dispatcher
// (exception_sync_curr_el) recognizes faults at those instructions,
// calls userland_demand_page to install the PTE if a VMA covers the
// page, and either resumes the faulting instruction (success) or
// transfers control to a fixup label that returns -1 (no VMA / perm
// denied / OOM).
//
// At v1.0 the only kernel-mode user-VA dereference is SYS_PUTS, which
// reads one byte at a time. We export a single primitive — uaccess_load_u8.
// Larger primitives (load_u16/u32/u64, store_*, copy_from_user) are
// added on demand; each lands its own fixup-table entry.
//
// The fixup table lives in a dedicated .uaccess_fixup section inside
// .rodata (per kernel.ld). Each entry is two u64s: (op_pc, fixup_pc).
// arch_fault_handle walks the table on kernel-mode user-VA faults via
// uaccess_fixup_lookup().

.section .text.uaccess, "ax"
.balign 4

// uaccess_load_u8 — read a single byte from a user VA.
//
// AArch64 ABI: x0 = user VA, x1 = u8 *out (kernel-direct-map pointer).
// Returns: x0 = 0 on success; x0 = -1 on fault (no VMA / perm denied /
//          OOM during demand-page sub-table alloc).
//
// On entry x0 must satisfy SYS_PUTS_USER_VA_TOP bound checks; the
// caller is responsible for VA-range validation. This primitive
// assumes x0 is a syntactically valid user VA.
//
// Implementation:
//   - ldrb of the user VA. If the page is not PTE-installed,
//     translation fault → exception_sync_curr_el → fixup table lookup
//     by ELR_EL1 → either demand-page-and-retry OR ELR := fault label
//     → branch to .L_uaccess_load_u8_fault below.
//   - strb to the kernel-direct-map output pointer.
//   - return 0.
.global uaccess_load_u8
.type uaccess_load_u8, @function
uaccess_load_u8:
    // Exported `uaccess_load_u8_op` + `uaccess_load_u8_fault` so the
    // in-kernel uaccess test can verify uaccess_fixup_lookup returns
    // these exact PCs (otherwise the labels are .L-local and live
    // only in the fixup table). Not consumed at runtime by the
    // dispatcher — it walks the table by PC equality alone.
.global uaccess_load_u8_op
uaccess_load_u8_op:
    ldrb    w2, [x0]                // FAULT POINT — table entry below
    strb    w2, [x1]
    mov     x0, xzr
    ret
.global uaccess_load_u8_fault
uaccess_load_u8_fault:
    mov     x0, #-1
    ret
.size uaccess_load_u8, . - uaccess_load_u8

// Fixup table entry. Linked into .uaccess_fixup section in .rodata
// (per kernel.ld). Each entry is two s32 PC-relative offsets: the
// op_pc and fixup_pc are derived by adding the offset to the
// address of the slot itself. PC-relative encoding avoids
// R_AARCH64_ABS64 relocations (rejected by the PIE-mode linker)
// and matches Linux's __ex_table convention. Walker decodes via
// `(u64)&slot + (s64)slot` per slot. uaccess_fixup_lookup consults
// this table on every kernel-mode user-VA fault.
//
// R12-uaccess F212 (P3) close: entry format is two .long (= 8 bytes)
// — MUST match `sizeof(struct uaccess_fixup_entry) == 8` pinned in
// arch/arm64/uaccess.c. If a future primitive needs a third field
// (Linux added a u8 type discriminant in 5.x's __ex_table), update
// the C struct + the asm emit pattern + the kernel.ld ALIGN
// directive in the same commit. The runtime test
// `uaccess.fixup_table_well_formed` checks `(end - start) % sizeof
// == 0` which catches a partial-entry drift but not a uniform
// per-entry size change — the format discipline is the primary
// protection.
.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_load_u8_op    - .
.long   uaccess_load_u8_fault - .
.popsection
