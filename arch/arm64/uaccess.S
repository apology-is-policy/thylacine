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

// uaccess_store_u8 — write a single byte to a user VA. The store
// equivalent of uaccess_load_u8; added at P5-fd-rw for SYS_READ's
// per-byte copy from kernel scratch into the user-VA buffer.
//
// AArch64 ABI: x0 = user VA, x1 = value to store (low byte of x1).
// Returns: x0 = 0 on success; x0 = -1 on fault (no VMA / perm denied /
//          OOM during demand-page sub-table alloc).
//
// On entry x0 must satisfy UACCESS_USER_VA_TOP bound checks; the
// caller is responsible for VA-range validation. This primitive
// assumes x0 is a syntactically valid user VA.
//
// Implementation mirrors uaccess_load_u8: a single strb is the
// fault point; the fixup-table entry maps it to the .L fault label
// below. The fault dispatcher (arch/arm64/fault.c) recognizes the
// kernel-mode user-VA write at this PC, calls userland_demand_page
// to install the PTE if a VMA permits writes, and either resumes
// the instruction or transfers control to the fault label.
.global uaccess_store_u8
.type uaccess_store_u8, @function
uaccess_store_u8:
.global uaccess_store_u8_op
uaccess_store_u8_op:
    strb    w1, [x0]                // FAULT POINT — table entry below
    mov     x0, xzr
    ret
.global uaccess_store_u8_fault
uaccess_store_u8_fault:
    mov     x0, #-1
    ret
.size uaccess_store_u8, . - uaccess_store_u8

.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_store_u8_op    - .
.long   uaccess_store_u8_fault - .
.popsection

// uaccess_load_u32 — read a single 32-bit word from a user VA. Added
// at P6-pouch-wait-addr for SYS_TORPOR_WAIT's futex-word compare: the
// kernel reads *user_va (the user's atomic-int) under torpor_lock and
// compares it against the caller's `expected`.
//
// AArch64 ABI: x0 = user VA, x1 = u32 *out (kernel-direct-map pointer).
// Returns: x0 = 0 on success (*out updated); x0 = -1 on fault.
//
// Caller MUST ensure x0 is 4-byte-aligned and < UACCESS_USER_VA_TOP;
// alignment is not checked here (an unaligned ldr would alignment-fault
// to the same fault dispatcher, but the fixup table catches only the
// expected translation/permission fault classes — keep the fast path
// honest by validating alignment in the C caller).
//
// Implementation mirrors uaccess_load_u8: one `ldr w2, [x0]` is the
// fault point; the fixup-table entry maps it to the `.L` fault label.
// Plain LDR (not LDAR) — a PARKING consumer's torpor_lock acquire pairs
// with the producer's torpor_lock release on its preceding WAKE to
// provide the memory ordering. Standard Linux-futex discipline. (The
// lock-free mismatch path — torpor.c's pre-lock compare — deliberately
// gets only this load's single-copy-atomicity; see torpor.h.)
.global uaccess_load_u32
.type uaccess_load_u32, @function
uaccess_load_u32:
.global uaccess_load_u32_op
uaccess_load_u32_op:
    ldr     w2, [x0]                // FAULT POINT — table entry below
    str     w2, [x1]
    mov     x0, xzr
    ret
.global uaccess_load_u32_fault
uaccess_load_u32_fault:
    mov     x0, #-1
    ret
.size uaccess_load_u32, . - uaccess_load_u32

.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_load_u32_op    - .
.long   uaccess_load_u32_fault - .
.popsection

// uaccess_store_u32 — write a single 32-bit word to a user VA. The
// store equivalent of uaccess_load_u32; added at P6-pouch-threads for
// the clear-child-tid handoff in SYS_THREAD_EXIT.
//
// AArch64 ABI: x0 = user VA, x1 = value (low 32 bits).
// Returns: x0 = 0 on success; x0 = -1 on fault (no VMA / perm denied /
//          OOM during demand-page sub-table alloc).
//
// Caller MUST ensure x0 is 4-byte-aligned and < UACCESS_USER_VA_TOP;
// alignment is not checked here (an unaligned STR would alignment-
// fault, which the fixup table does NOT catch — keep the fast path
// honest by validating alignment in the C caller).
//
// Plain STR (not STLR) — the producer-side store; the consumer's later
// load runs under torpor_lock whose acquire pairs with the lock-release
// following this store. Mirrors uaccess_load_u32's discipline.
.global uaccess_store_u32
.type uaccess_store_u32, @function
uaccess_store_u32:
.global uaccess_store_u32_op
uaccess_store_u32_op:
    str     w1, [x0]                // FAULT POINT — table entry below
    mov     x0, xzr
    ret
.global uaccess_store_u32_fault
uaccess_store_u32_fault:
    mov     x0, #-1
    ret
.size uaccess_store_u32, . - uaccess_store_u32

.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_store_u32_op    - .
.long   uaccess_store_u32_fault - .
.popsection

// uaccess_copy_out — copy `len` bytes from a kernel buffer to a user VA.
//
// CF-3 A: the bulk replacement for the per-byte uaccess_store_u8 loops in
// the byte-I/O syscall handlers (SYS_READ / SYS_PREAD copy-out). One call
// is one fault-recovery domain: a translation/permission fault on any of
// the user-side stores lands on the shared fault label and returns -1.
//
// AArch64 ABI: x0 = user dst VA, x1 = kernel src (direct-map), x2 = len.
// Returns: x0 = 0 on success; x0 = -1 on fault (no VMA / perm denied /
//          OOM during demand-page sub-table alloc).
//
// Caller contract mirrors uaccess_store_u8: [x0, x0+len) must lie in
// [0, UACCESS_USER_VA_TOP) (sys_validate_user_buf gates the syscall
// surface); x1 must be kernel-readable for len bytes.
//
// Shape: byte head until the USER VA is 8-aligned, 8-byte body, byte
// tail. The kernel-side ldr may be unaligned (Normal memory,
// SCTLR_EL1.A = 0 per INIT_SCTLR_EL1_MMU_OFF = 0x30D00800). The faulting
// stores use NO writeback: on a demand-page resume the re-executed store
// sees unchanged registers. On a mid-copy fault (page N mapped, page N+1
// not) bytes already stored stay stored -- the caller returns -1 and the
// syscall's documented partial-copy caveat applies (unchanged from the
// per-byte era, which had the same property).
.global uaccess_copy_out
.type uaccess_copy_out, @function
uaccess_copy_out:
    cbz     x2, .Lcpo_ok
.Lcpo_head:
    tst     x0, #7
    b.eq    .Lcpo_body
    ldrb    w3, [x1]
.global uaccess_copy_out_op_head
uaccess_copy_out_op_head:
    strb    w3, [x0]                // FAULT POINT — table entry below
    add     x0, x0, #1
    add     x1, x1, #1
    subs    x2, x2, #1
    b.ne    .Lcpo_head
    b       .Lcpo_ok
.Lcpo_body:
    cmp     x2, #8
    b.lo    .Lcpo_tail
    ldr     x3, [x1]
.global uaccess_copy_out_op_body
uaccess_copy_out_op_body:
    str     x3, [x0]                // FAULT POINT — table entry below
    add     x0, x0, #8
    add     x1, x1, #8
    sub     x2, x2, #8
    b       .Lcpo_body
.Lcpo_tail:
    cbz     x2, .Lcpo_ok
    ldrb    w3, [x1]
.global uaccess_copy_out_op_tail
uaccess_copy_out_op_tail:
    strb    w3, [x0]                // FAULT POINT — table entry below
    add     x0, x0, #1
    add     x1, x1, #1
    sub     x2, x2, #1
    b       .Lcpo_tail
.Lcpo_ok:
    mov     x0, xzr
    ret
.global uaccess_copy_out_fault
uaccess_copy_out_fault:
    mov     x0, #-1
    ret
.size uaccess_copy_out, . - uaccess_copy_out

.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_copy_out_op_head  - .
.long   uaccess_copy_out_fault    - .
.long   uaccess_copy_out_op_body  - .
.long   uaccess_copy_out_fault    - .
.long   uaccess_copy_out_op_tail  - .
.long   uaccess_copy_out_fault    - .
.popsection

// uaccess_copy_in — copy `len` bytes from a user VA to a kernel buffer.
//
// The mirror of uaccess_copy_out for the write-side staging (SYS_WRITE /
// SYS_PWRITE copy-in): here the user-side LOADS are the fault points; the
// kernel-side stores may be unaligned (same SCTLR_EL1.A = 0 argument).
//
// AArch64 ABI: x0 = kernel dst (direct-map), x1 = user src VA, x2 = len.
// Returns: x0 = 0 on success; x0 = -1 on fault.
//
// Caller contract: [x1, x1+len) must lie in [0, UACCESS_USER_VA_TOP)
// (sys_validate_user_buf); x0 must be kernel-writable for len bytes. On a
// mid-copy fault the kernel buffer's already-copied prefix is defined,
// the rest is unspecified -- callers treat -1 as whole-op EFAULT and must
// not consume the buffer.
.global uaccess_copy_in
.type uaccess_copy_in, @function
uaccess_copy_in:
    cbz     x2, .Lcpi_ok
.Lcpi_head:
    tst     x1, #7
    b.eq    .Lcpi_body
.global uaccess_copy_in_op_head
uaccess_copy_in_op_head:
    ldrb    w3, [x1]                // FAULT POINT — table entry below
    strb    w3, [x0]
    add     x0, x0, #1
    add     x1, x1, #1
    subs    x2, x2, #1
    b.ne    .Lcpi_head
    b       .Lcpi_ok
.Lcpi_body:
    cmp     x2, #8
    b.lo    .Lcpi_tail
.global uaccess_copy_in_op_body
uaccess_copy_in_op_body:
    ldr     x3, [x1]                // FAULT POINT — table entry below
    str     x3, [x0]
    add     x0, x0, #8
    add     x1, x1, #8
    sub     x2, x2, #8
    b       .Lcpi_body
.Lcpi_tail:
    cbz     x2, .Lcpi_ok
.global uaccess_copy_in_op_tail
uaccess_copy_in_op_tail:
    ldrb    w3, [x1]                // FAULT POINT — table entry below
    strb    w3, [x0]
    add     x0, x0, #1
    add     x1, x1, #1
    sub     x2, x2, #1
    b       .Lcpi_tail
.Lcpi_ok:
    mov     x0, xzr
    ret
.global uaccess_copy_in_fault
uaccess_copy_in_fault:
    mov     x0, #-1
    ret
.size uaccess_copy_in, . - uaccess_copy_in

.pushsection .uaccess_fixup, "a", @progbits
.balign 4
.long   uaccess_copy_in_op_head  - .
.long   uaccess_copy_in_fault    - .
.long   uaccess_copy_in_op_body  - .
.long   uaccess_copy_in_fault    - .
.long   uaccess_copy_in_op_tail  - .
.long   uaccess_copy_in_fault    - .
.popsection
