// arch/arm64/context.S — context-switch primitive (P2-A; extended P4-Ic5-FP).
//
// cpu_switch_context(struct Context *prev /* x0 */, struct Context *next /* x1 */)
//
// Saves the AAPCS64 callee-saved registers (x19-x28, x29 FP, x30 LR)
// plus SP and TPIDR_EL0 into `prev`; loads the same fields from `next`
// into the live CPU; rets to the loaded LR.
//
// This routine is a function-call leaf: no nested call, no stack frame
// of its own. x30 at entry is the bl return address (unsigned per
// AAPCS64); we save it directly into prev->lr without paciasp / autiasp.
// The caller's pac-ret discipline pushes its OWN signed lr to its stack
// at its prologue; what we hold in x30 across the call is just the fresh
// bl return address. See context.h commentary for the discipline.
//
// Field offsets MUST match struct Context layout in <thylacine/context.h>:
//   x19 @ 0   x20 @ 8    x21 @ 16   x22 @ 24
//   x23 @ 32  x24 @ 40   x25 @ 48   x26 @ 56
//   x27 @ 64  x28 @ 72   fp  @ 80   lr  @ 88
//   sp  @ 96  tpidr_el0 @ 104       ttbr0 @ 112
//   _pad_fp  @ 120 (alignment pad to 16-byte boundary)
//   fp_v[0..31] @ 128 (16 STP-Q pairs at 0/32/64/.../480 from fp_v base)
//   fpsr @ 640  fpcr @ 644  (struct size 656, 8-byte trailing pad to 16)
//
// _Static_asserts in context.h pin both sides; a layout drift triggers a
// build error there before the asm can corrupt context switches.
//
// P3-Bdb: ttbr0 swap. Each Thread's Context carries the TTBR0_EL1 value
// (ASID<<48 | pgtable_root_PA) appropriate for the Thread's Proc.
// cpu_switch_context saves the live TTBR0_EL1 into prev (atomic with the
// other register saves) and loads next's TTBR0 into the live CPU. An ISB
// after MSR TTBR0_EL1 makes the change visible to subsequent fetches.
// No TLB invalidate is needed: per-Proc ASIDs (1..255) tag user-mappings
// in the TLB; cross-Proc TLB entries don't collide. Kernel mappings
// (TTBR1, global, ASID-agnostic) are unaffected.
//
// P4-Ic5-FP: eager save/restore of FP/SIMD state. Q0..Q31 + FPSR + FPCR
// saved into prev and loaded from next every context switch. The kernel
// is compiled with -mgeneral-regs-only so kernel C code doesn't touch
// V registers; what we save here is whatever EL0 left in V regs at its
// most recent EL→EL1 transition (preserved across kernel-mode execution
// because the kernel never clobbers V regs). CPACR_EL1.FPEN must be
// 0b11 before this code runs — fp_enable_this_cpu (context.h inline) is
// called from boot_main + per_cpu_main to set it before any context
// switch.
//
// Why eager not lazy: simpler. Lazy needs per-CPU "current FP owner"
// tracking + CPACR_EL1.FPEN trap-and-allocate machinery; eager saves
// 528 bytes per thread inline and always does the round-trip. At v1.0
// thread counts (< 100 alive at once) the RSS impact is < 100 KiB total.
// Phase 5+ may switch to lazy if profiling shows the save/restore cost
// matters.

// Enable FP/SIMD feature for the assembler in this TU. The kernel
// toolchain is compiled with -mgeneral-regs-only so the C-side doesn't
// emit FP/SIMD instructions, but the cpu_switch_context save/restore
// path explicitly handles V0..V31 + FPSR + FPCR. `.arch_extension fp`
// is additive over the prevailing `-march=armv8-a+lse+...` profile;
// it doesn't override hardening features. Per Arm GAS reference 6.3.
.arch_extension fp

.section ".text", "ax"

.globl cpu_switch_context
.type cpu_switch_context, %function
cpu_switch_context:
    // Per ARM ARM D24.2.1: BL is a direct branch and sets PSTATE.BTYPE
    // = 0b00 (no constraint). BLR (which a future indirect-call site
    // might use to reach us) sets BTYPE = 0b10 (call). bti c matches
    // both: it passes under BTYPE=00 (any landing allowed) and under
    // BTYPE=10 (call landing). Direct BL from C → BTYPE=00 is the
    // common case at P2-A.
    bti     c

    // ----- Save prev (x0) -----

    // Save callee-saved + FP + LR.
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]

    // SP can't be a stp source — read via mov to a scratch GPR first.
    mov     x9, sp
    str     x9, [x0, #96]

    mrs     x9, tpidr_el0
    str     x9, [x0, #104]

    // P3-Bdb: save current TTBR0_EL1 into prev->ctx.ttbr0.
    mrs     x9, ttbr0_el1
    str     x9, [x0, #112]

    // P4-Ic5-FP: save V0..V31 + FPSR + FPCR into prev's fp region.
    // x9 := &prev->fp_v[0] (offset 128 from prev base). STP (SIMD&FP)
    // signed-7 immediate × 16 → max +1008; we touch up to +480 here.
    add     x9, x0, #128
    stp     q0,  q1,  [x9, #0]
    stp     q2,  q3,  [x9, #32]
    stp     q4,  q5,  [x9, #64]
    stp     q6,  q7,  [x9, #96]
    stp     q8,  q9,  [x9, #128]
    stp     q10, q11, [x9, #160]
    stp     q12, q13, [x9, #192]
    stp     q14, q15, [x9, #224]
    stp     q16, q17, [x9, #256]
    stp     q18, q19, [x9, #288]
    stp     q20, q21, [x9, #320]
    stp     q22, q23, [x9, #352]
    stp     q24, q25, [x9, #384]
    stp     q26, q27, [x9, #416]
    stp     q28, q29, [x9, #448]
    stp     q30, q31, [x9, #480]
    // FPSR and FPCR are 32-bit; mrs reads the low 32 bits of an Xn.
    // FPSR (S3_3_C4_C4_1) + FPCR (S3_3_C4_C4_0). Some assembler versions
    // reject the `msr FPSR/FPCR` mnemonic for write paths; use the
    // S<op0>_<op1>_C<CRn>_C<CRm>_<op2> generic form which is universally
    // accepted.
    mrs     x10, S3_3_C4_C4_1       // FPSR
    mrs     x11, S3_3_C4_C4_0       // FPCR
    str     w10, [x0, #640]
    str     w11, [x0, #644]

    // ----- Load next (x1) -----

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]

    ldr     x9, [x1, #96]
    mov     sp, x9

    ldr     x9, [x1, #104]
    msr     tpidr_el0, x9

    // P3-Bdb: load next's TTBR0_EL1. Encoded as (ASID<<48) | pgtable_root.
    // ISB serializes the TTBR0 change so subsequent translations / TLB
    // walks see the new value. No TLB invalidate needed: ASID tags user
    // mappings in the TLB; cross-Proc TLB entries don't collide. Kernel
    // mappings via TTBR1 are global and unaffected.
    ldr     x9, [x1, #112]
    msr     ttbr0_el1, x9
    isb

    // P4-Ic5-FP: load V0..V31 + FPSR + FPCR from next's fp region.
    add     x9, x1, #128
    ldp     q0,  q1,  [x9, #0]
    ldp     q2,  q3,  [x9, #32]
    ldp     q4,  q5,  [x9, #64]
    ldp     q6,  q7,  [x9, #96]
    ldp     q8,  q9,  [x9, #128]
    ldp     q10, q11, [x9, #160]
    ldp     q12, q13, [x9, #192]
    ldp     q14, q15, [x9, #224]
    ldp     q16, q17, [x9, #256]
    ldp     q18, q19, [x9, #288]
    ldp     q20, q21, [x9, #320]
    ldp     q22, q23, [x9, #352]
    ldp     q24, q25, [x9, #384]
    ldp     q26, q27, [x9, #416]
    ldp     q28, q29, [x9, #448]
    ldp     q30, q31, [x9, #480]
    ldr     w10, [x1, #640]
    ldr     w11, [x1, #644]
    msr     S3_3_C4_C4_1, x10       // FPSR
    msr     S3_3_C4_C4_0, x11       // FPCR
    // R12-FP audit close (F167 P3): isb after FPCR write so subsequent
    // FP instructions observe the new rounding-mode / trap-enable bits.
    // ARM ARM D8.7.5: "Direct writes to FPCR can be synchronized using
    // an ISB instruction." Without ISB, post-switch FP instructions
    // (in-kernel: none today; out-to-EL0 via eret: context-synchronizing
    // by D7.1.3 K11.2.1 so unaffected) could observe stale FPCR until
    // the next context-synchronizing event. Forward-defense for Phase 5+
    // in-kernel FP usage (NEON memcpy variants, NEON checksums, AES
    // intrinsics) — would otherwise produce subtle wrong-rounding bugs.
    isb

    ret
.size cpu_switch_context, . - cpu_switch_context


// thread_trampoline — initial ctx.lr for fresh threads created by
// thread_create / thread_create_with_arg. Reached via cpu_switch_context's
// ret. Per ARM ARM D24.2.1, RET sets PSTATE.BTYPE = 0b00 (no constraint)
// — the first instruction's bti c here passes regardless. bti c is
// defensive: it also passes if some future caller reaches the trampoline
// via blr (BTYPE=10).
//
// x21 carries the entry function pointer, parked by thread_create via
// ctx.x21 and loaded by cpu_switch_context's `ldp x21,x22`. x20 carries
// the entry's arg (parked by thread_create_with_arg via ctx.x20; zero
// for thread_create no-arg entries via KP_ZERO). The mov x0, x20 below
// places the arg in x0 per AAPCS64; entries that take no arg ignore
// x0 (it's caller-saved). The blr sets BTYPE=10 (call); the entry
// function's compiler-emitted `bti c` prologue (under
// -mbranch-protection=pac-ret+bti) matches.
//
// IRQ-mask discipline (P2-Bc): the schedule that switched into this
// fresh thread had IRQs masked (sched() entry's irqsave; cpu_switch_
// context preserves PSTATE.DAIF). For a fresh thread with no prior
// sched() frame to call irqrestore, we unmask IRQs explicitly here
// so timer-IRQ-driven preemption can fire while the thread runs its
// entry function. Subsequent sched() calls from the entry restore
// IRQ state via the normal irqsave/irqrestore pattern; no further
// asymmetry. `msr daifclr, #2` clears PSTATE.I (IRQ mask bit) only —
// the other DAIF bits stay at whatever the prior state was.
//
// If entry returns, halt on WFE — P2-D introduces exits() as the
// structured termination path; entry returning without exits() is a
// kernel-thread bug.
.globl thread_trampoline
.type thread_trampoline, %function
thread_trampoline:
    bti     c

    /* P2-Ce: release the run-tree lock prev held when switching to us.
     * Mirrors the post-cpu_switch_context lock-release in sched()'s
     * resume path. Without this, the per-CPU lock stays held forever
     * (until the new thread's own first sched() call, which would
     * then double-acquire and deadlock).
     *
     * sched_finish_task_switch is a leaf C function — calling it
     * before unmask is fine: PAC was applied per-CPU at start.S
     * before the long-branch, and bl preserves x21 (entry fn ptr)
     * AND x20 (entry arg) since both are callee-saved per AAPCS64. */
    bl      sched_finish_task_switch

    msr     daifclr, #2          /* P2-Bc: unmask IRQs for new thread */

    mov     x0, x20              /* P2-D: pass arg to entry. zero for
                                  *      no-arg entries via KP_ZERO. */
    blr     x21

    // Entry returned without calling exits() (P2-D). Halt.
1:  wfe
    b       1b
.size thread_trampoline, . - thread_trampoline


// thread_user_trampoline — user-mode variant of thread_trampoline.
// P6-pouch-threads (sub-chunk 9). Used as the initial ctx.lr for Threads
// created by thread_create_user (the kernel-side of SYS_THREAD_SPAWN).
//
// Same sched_finish_task_switch release as thread_trampoline, but the
// DAIF discipline DIFFERS (P6 #713): the kernel thread_trampoline UNmasks
// IRQs before `blr x21` (a kernel entry stays at EL1 and must be
// preemptible immediately); this user trampoline instead keeps DAIF
// MASKED through the eret, because the eret restores SPSR_EL1=0 and that
// is what makes the new thread preemptible AT EL0 -- atomically, with no
// interruptible ELR-set..eret window. The tail differs too: instead of
// `blr x21` to a kernel entry function, this trampoline performs the
// userland_enter eret dance inline, consuming four args parked in
// callee-saved ctx slots that cpu_switch_context loaded just before
// `ret` into here:
//
//   x19 = user_sp_va    — SP_EL0 install (non-current bank write at EL1h)
//   x20 = user_arg      — moved to x0 (AAPCS64 arg-0) for the user entry
//   x21 = user_entry_va — ELR_EL1 target
//   x22 = user_tls_va   — TPIDR_EL0 init value (0 = no TLS yet, valid)
//
// PSTATE for the eret target: EL0t, all DAIF clear — same as
// userland_enter's (SPSR_EL1 = 0). Every GPR except x0 is zeroed so no
// kernel state leaks across the EL1→EL0 boundary; x0 carries `arg` per
// AAPCS64 so the user entry function receives it correctly.
//
// SP_EL0 install at EL1h: the kernel runs uniformly at EL1h (SPSel=1,
// invariant I-21), so the live `sp` is SP_EL1 (the calling thread's
// kstack). Writing SP_EL0 via `msr sp_el0, x19` is the architecturally
// well-defined non-current-bank write (NOT the CONSTRAINED UNPREDICTABLE
// write-the-current-SP form, which would only fire at EL1t). Matches
// the exact discipline arch/arm64/userland.S documents.
.globl thread_user_trampoline
.type thread_user_trampoline, %function
thread_user_trampoline:
    bti     c

    /* Release the run-tree lock prev held when switching to us — same
     * discipline as thread_trampoline; mirrors the sched() resume path. */
    bl      sched_finish_task_switch

    /* P6 #811 (#809-audit F3): first-entry group-termination die-check. A peer
     * Thread spawned while its Proc is already group-terminating would
     * otherwise execute EL0 code until its next trap; check HERE so it
     * self-terminates before ever reaching EL0 (closes I-24's "no Thread runs
     * at EL0 after ZOMBIE" for the freshly-spawned-during-termination case).
     * el0_return_die_check reads current->proc->group_exit_msg and, if set,
     * calls thread_exit_self -> sched() (noreturn) -- so the die path NEVER
     * reaches the ELR-set..eret window below (#713-safe: no interruptible
     * ELR_EL1-set window is entered on the die path). x19..x22 (user
     * sp/arg/entry/tls) are callee-saved across this C call, so they survive
     * into the msr operands. Same C-ABI footing as sched_finish_task_switch
     * above; runs BEFORE the daifset window. */
    bl      el0_return_die_check

    /* P6 #713: keep ALL of DAIF MASKED through the eret. Pre-fix this was
     * `msr daifclr, #2` (UNMASK IRQ) placed BEFORE `msr elr_el1` -- so an
     * IRQ taken in the ELR-set..eret window re-entered the exception path
     * and OVERWROTE ELR_EL1 with the interrupted kernel PC (this trampoline
     * + ~0x10). This trampoline never re-sets ELR after that point, so the
     * eret then returned EL0 to that kernel address -> a rare instruction-
     * permission fault in a freshly-spawned Proc (#713 root cause). The
     * eret below restores SPSR_EL1=0 (DAIF clear), so the new thread still
     * runs with IRQs ON at EL0 -- preemptibility is established ATOMICALLY
     * by the eret, not by a vulnerable pre-eret window. */
    msr     daifset,   #0xf       /* mask D/A/I/F until the eret */

    /* Stage user-mode entry: park entry_pc / sp into scratch x16/x17 so
     * the GPR-zero sweep below can freely clobber x0..x18. x19..x22 are
     * read directly so they survive into the msr operands. */
    msr     elr_el1,   x21        /* ELR_EL1 := user_entry_va */
    msr     spsr_el1,  xzr        /* SPSR_EL1 := 0 (EL0t, DAIF clear) */
    msr     sp_el0,    x19        /* SP_EL0 := user_sp_va (non-current bank) */
    msr     tpidr_el0, x22        /* TPIDR_EL0 := user_tls_va (0 if none) */

    /* x0 := user_arg — AAPCS64 arg-0 for the user entry function.
     * Established AFTER the msr stack of args so the four msr operands
     * have already been consumed; safe to clobber x20 in the sweep. */
    mov     x0, x20

    /* Zero every other GPR — no kernel state crosses EL1→EL0. */
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr             /* FP */
    mov     x30, xzr             /* LR */

    eret

    /* Defensive infinite loop if eret somehow falls through. */
1:  wfe
    b       1b
.size thread_user_trampoline, . - thread_user_trampoline
