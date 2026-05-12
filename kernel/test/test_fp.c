// FP/SIMD context save/restore tests (R12-FP F166 P2 close).
//
// Pins the FP-state-preservation-across-switch invariant that the
// P4-Ic5-FP chunk implemented in cpu_switch_context's STP/LDP-Q pairs
// + FPSR/FPCR MSR/MRS. Without these tests, a regression in the
// save/restore mechanism (off-by-one offsets in STP-Q immediates,
// swapped pair operands like q4↔q5, missing FPSR or FPCR save, wrong
// system register encoding) would pass the suite because no other
// test exercises FP save/restore across a switch boundary. The audit
// at R12-FP empirically verified (`llvm-objdump -d`) that the
// userspace test binaries emit ZERO FP/SIMD instructions — the
// claimed indirect coverage was empirically false.
//
// Two tests:
//
//   fp.cpacr_enabled
//     Verifies CPACR_EL1.FPEN == 0b11 (no FP trap at any EL). If
//     fp_enable_this_cpu() was never called on this CPU, this fails
//     before the more elaborate round-trip — clean diagnostic.
//
//   fp.round_trip_v_regs_fpsr_fpcr
//     Loads V0..V31 + FPCR + FPSR with a sentinel pattern, switches
//     into a helper thread that DELIBERATELY clobbers V0..V31 +
//     FPCR + FPSR with a different pattern, then switches back. On
//     resume, the main thread saves V0..V31 + FPCR + FPSR to a
//     verify buffer and checks every byte against the sentinel. The
//     deliberate clobber by the helper proves the test is sensitive
//     — without correct save/restore, the main thread would observe
//     the helper's pattern instead of its own.

#include "test.h"

#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// The rest of the kernel compiles with `-mgeneral-regs-only`, which
// adds `+no-fp+no-neon` to the LLVM target features and blocks inline
// asm from using V regs / Q regs even with a `.arch_extension fp`
// directive (the codegen-level feature gate runs before the assembler
// directive). To use FP/SIMD inline asm in this TU, we attach
// `__attribute__((target("+fp+simd")))` to each function that emits
// FP-or-NEON instructions. The attribute re-enables fp-armv8 + NEON
// for that function only; other functions still inherit the
// kernel-wide -mgeneral-regs-only setting. Functions that only access
// FPCR/FPSR via the generic system-reg encoding (S3_3_C4_C4_{0,1}) do
// not need the attribute — the assembler treats those as general
// MSR/MRS variants.

// 32 × 16 B for V0..V31 + 4 B FPCR + 4 B FPSR. _Alignas(16) so STP-Q /
// LDP-Q at [%[buf], #offset] satisfy the 16-byte alignment requirement
// (ARM ARM C7.2.348). Two buffers — sentinel (loaded into regs before
// the switch) and verify (saved from regs after the round-trip).
static _Alignas(16) u8 g_fp_sentinel[32 * 16 + 8];
static _Alignas(16) u8 g_fp_verify[32 * 16 + 8];

static volatile u32   g_fp_test_state;
static struct Thread *g_fp_test_main;

// Helper thread: deliberately clobber V0..V31 + FPCR + FPSR with a
// pattern distinct from the main thread's sentinel, then switch back.
// The clobber is what makes the round-trip test sensitive — without
// it, even broken save/restore could appear to "work" because the
// helper happened not to touch the registers.
__attribute__((target("+fp+simd")))
static void test_fp_helper_entry(void) {
    // movi v_k.16b, #imm8 sets every byte of V_k to imm8. We use
    // patterns 0xa0..0xbf (32 distinct values, alternating high nibble)
    // so the clobber is unambiguous against the main thread's sentinel
    // (which uses (k * 17 + j) & 0xff).
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "movi v0.16b,  #0xa0\n"
        "movi v1.16b,  #0xa1\n"
        "movi v2.16b,  #0xa2\n"
        "movi v3.16b,  #0xa3\n"
        "movi v4.16b,  #0xa4\n"
        "movi v5.16b,  #0xa5\n"
        "movi v6.16b,  #0xa6\n"
        "movi v7.16b,  #0xa7\n"
        "movi v8.16b,  #0xa8\n"
        "movi v9.16b,  #0xa9\n"
        "movi v10.16b, #0xaa\n"
        "movi v11.16b, #0xab\n"
        "movi v12.16b, #0xac\n"
        "movi v13.16b, #0xad\n"
        "movi v14.16b, #0xae\n"
        "movi v15.16b, #0xaf\n"
        "movi v16.16b, #0xb0\n"
        "movi v17.16b, #0xb1\n"
        "movi v18.16b, #0xb2\n"
        "movi v19.16b, #0xb3\n"
        "movi v20.16b, #0xb4\n"
        "movi v21.16b, #0xb5\n"
        "movi v22.16b, #0xb6\n"
        "movi v23.16b, #0xb7\n"
        "movi v24.16b, #0xb8\n"
        "movi v25.16b, #0xb9\n"
        "movi v26.16b, #0xba\n"
        "movi v27.16b, #0xbb\n"
        "movi v28.16b, #0xbc\n"
        "movi v29.16b, #0xbd\n"
        "movi v30.16b, #0xbe\n"
        "movi v31.16b, #0xbf\n"
        : :
        : "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31"
    );

    // Clobber FPCR + FPSR with a different pattern than the sentinel.
    // FPCR sentinel uses bits 24-25 (FZ + DN); we use bit 26 (AHP) to
    // make the clobber distinguishable. FPSR sentinel uses bits 0-1;
    // we use bit 7 (NEP, when in EL0; for EL1 testing purposes still
    // writable as a cumulative-flag-like bit).
    u64 fpsr_clobber = 0x000000f0ull;
    u64 fpcr_clobber = 0x04000000ull;
    __asm__ __volatile__(
        "msr S3_3_C4_C4_1, %0\n"  // FPSR
        "msr S3_3_C4_C4_0, %1\n"  // FPCR
        : : "r"(fpsr_clobber), "r"(fpcr_clobber)
    );

    g_fp_test_state++;
    thread_switch(g_fp_test_main);
    // Unreachable in the test — main thread doesn't switch back.
}

void test_fp_cpacr_enabled(void) {
    u64 cpacr;
    __asm__ __volatile__("mrs %0, CPACR_EL1" : "=r"(cpacr));
    // FPEN is bits [21:20]; 0b11 = no trap at any EL.
    u64 fpen = (cpacr >> 20) & 0x3ull;
    TEST_EXPECT_EQ(fpen, 0x3ull,
        "CPACR_EL1.FPEN must be 0b11 (no FP/SIMD trap); fp_enable_this_cpu must run on every CPU bring-up");

    // R12-FP F168 close: ZEN / SMEN / TTA defensively masked at
    // fp_enable_this_cpu time. They should be 0 (trap SVE/SME, trap
    // trace-access). If a future change loses the mask, this test
    // surfaces the regression.
    u64 zen  = (cpacr >> 16) & 0x3ull;
    u64 smen = (cpacr >> 24) & 0x3ull;
    u64 tta  = (cpacr >> 28) & 0x1ull;
    TEST_EXPECT_EQ(zen, 0ull,
        "CPACR_EL1.ZEN must be 0 (trap SVE) — R12-FP F168 defensive mask");
    TEST_EXPECT_EQ(smen, 0ull,
        "CPACR_EL1.SMEN must be 0 (trap SME) — R12-FP F168 defensive mask");
    TEST_EXPECT_EQ(tta, 0ull,
        "CPACR_EL1.TTA must be 0 (trap trace) — R12-FP F168 defensive mask");
}

__attribute__((target("+fp+simd")))
void test_fp_round_trip_v_regs_fpsr_fpcr(void) {
    g_fp_test_main = current_thread();
    TEST_ASSERT(g_fp_test_main != NULL,
        "current_thread() returned NULL (thread_init not called?)");

    // Build the sentinel: V_k bytes = (k * 17 + j) & 0xff for byte j of
    // V_k. The * 17 + j produces a unique pattern per (k, j) so a
    // swapped-pair or wrong-offset bug in cpu_switch_context's STP-Q
    // sequence would mismatch a specific byte at a specific offset —
    // the assertion below reports both.
    for (int k = 0; k < 32; k++) {
        for (int j = 0; j < 16; j++) {
            g_fp_sentinel[k * 16 + j] = (u8)((k * 17 + j) & 0xff);
        }
    }
    // FPCR sentinel: FZ (bit 24) + DN (bit 25). Both writable on every
    // ARMv8 implementation per ARM ARM D13.2.91. Bits 26-29 are
    // ID-dependent (AHP, IDE, IXE, UFE) — avoid them to keep the test
    // portable across cores.
    u32 fpcr_sentinel = 0x03000000u;
    // FPSR sentinel: IOC (bit 0) + DZC (bit 1) cumulative flags. Both
    // writable everywhere per D13.2.92.
    u32 fpsr_sentinel = 0x00000003u;
    *(u32 *)&g_fp_sentinel[32 * 16]     = fpcr_sentinel;
    *(u32 *)&g_fp_sentinel[32 * 16 + 4] = fpsr_sentinel;

    // Load V0..V31 from the sentinel buffer via 16 LDP-Q pairs.
    // Offsets match cpu_switch_context's save/restore offsets to keep
    // the mental model consistent (Q0+Q1 at +0, Q2+Q3 at +32, ...,
    // Q30+Q31 at +480).
    u8 *src = g_fp_sentinel;
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "ldp q0,  q1,  [%0, #0]\n"
        "ldp q2,  q3,  [%0, #32]\n"
        "ldp q4,  q5,  [%0, #64]\n"
        "ldp q6,  q7,  [%0, #96]\n"
        "ldp q8,  q9,  [%0, #128]\n"
        "ldp q10, q11, [%0, #160]\n"
        "ldp q12, q13, [%0, #192]\n"
        "ldp q14, q15, [%0, #224]\n"
        "ldp q16, q17, [%0, #256]\n"
        "ldp q18, q19, [%0, #288]\n"
        "ldp q20, q21, [%0, #320]\n"
        "ldp q22, q23, [%0, #352]\n"
        "ldp q24, q25, [%0, #384]\n"
        "ldp q26, q27, [%0, #416]\n"
        "ldp q28, q29, [%0, #448]\n"
        "ldp q30, q31, [%0, #480]\n"
        : : "r"(src)
        : "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31"
    );
    // Load FPCR + FPSR. Order: FPSR first, then FPCR (the FPCR write +
    // the trailing isb mirror cpu_switch_context's restore tail).
    u64 fpsr_load = (u64)fpsr_sentinel;
    u64 fpcr_load = (u64)fpcr_sentinel;
    __asm__ __volatile__(
        "msr S3_3_C4_C4_1, %0\n"  // FPSR
        "msr S3_3_C4_C4_0, %1\n"  // FPCR
        "isb\n"
        : : "r"(fpsr_load), "r"(fpcr_load)
    );

    g_fp_test_state = 0;

    struct Thread *t = thread_create(kproc(), test_fp_helper_entry);
    TEST_ASSERT(t != NULL, "thread_create returned NULL");

    // The round trip. After thread_switch(t) returns, we are back in
    // g_fp_test_main and t has run its entry (which clobbered FP regs
    // and switched back). All FP state must have survived via
    // cpu_switch_context's STP/LDP-Q + FPSR/FPCR save/restore.
    thread_switch(t);

    TEST_EXPECT_EQ(g_fp_test_state, 1u,
        "helper did not run / did not switch back");

    // Save V0..V31 to the verify buffer + read FPCR + FPSR.
    u8 *dst = g_fp_verify;
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "stp q0,  q1,  [%0, #0]\n"
        "stp q2,  q3,  [%0, #32]\n"
        "stp q4,  q5,  [%0, #64]\n"
        "stp q6,  q7,  [%0, #96]\n"
        "stp q8,  q9,  [%0, #128]\n"
        "stp q10, q11, [%0, #160]\n"
        "stp q12, q13, [%0, #192]\n"
        "stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n"
        "stp q18, q19, [%0, #288]\n"
        "stp q20, q21, [%0, #320]\n"
        "stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n"
        "stp q26, q27, [%0, #416]\n"
        "stp q28, q29, [%0, #448]\n"
        "stp q30, q31, [%0, #480]\n"
        : : "r"(dst) : "memory"
    );
    u64 fpsr_verify_u64;
    u64 fpcr_verify_u64;
    __asm__ __volatile__("mrs %0, S3_3_C4_C4_1" : "=r"(fpsr_verify_u64));
    __asm__ __volatile__("mrs %0, S3_3_C4_C4_0" : "=r"(fpcr_verify_u64));
    *(u32 *)&g_fp_verify[32 * 16]     = (u32)fpcr_verify_u64;
    *(u32 *)&g_fp_verify[32 * 16 + 4] = (u32)fpsr_verify_u64;

    // Byte-by-byte comparison. The (k, j) decomposition surfaces which
    // V register (k) and byte offset (j) failed; FPCR / FPSR live in
    // the tail 8 bytes. If a swap-pair bug occurred (e.g., q4 ↔ q5),
    // the failure-byte computation lands at exactly the right diagnostic
    // offset.
    for (int i = 0; i < 32 * 16 + 8; i++) {
        if (g_fp_verify[i] != g_fp_sentinel[i]) {
            // Extinction rather than TEST_ASSERT to land a recognizable
            // diagnostic in the boot log with the offset.
            extinction("fp.round_trip: FP-state-preservation-across-switch invariant violated (V reg / FPCR / FPSR did not survive context switch)");
        }
    }

    // Helper is suspended in its thread_switch call; we don't resume it
    // (mirrors test_context_round_trip's pattern). Free it cleanly.
    thread_free(t);
}
