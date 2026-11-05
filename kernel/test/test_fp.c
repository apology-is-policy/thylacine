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
// Tests:
//
//   fp.cpacr_enabled
//     Verifies CPACR_EL1.FPEN == 0b11 (no FP trap at any EL). If
//     fp_enable_this_cpu() was never called on this CPU, this fails
//     before the more elaborate round-trip — clean diagnostic.
//
//   fp.round_trip_v_regs_fpsr_fpcr
//     One thread of a Proc loads V0..V31 + FPCR + FPSR with a sentinel and
//     switches to a thread that DELIBERATELY clobbers them, once of the same
//     Proc and once of another, then back. Kernel threads hold no V state, so
//     both must be on an address space for the switch to save anything. The
//     clobber proves the test is sensitive -- without correct save/restore the
//     first thread would observe the second's pattern.
//
//   fp.kernel_thread_holds_no_v
//   fp.user_v_survives_a_kernel_thread
//     Neither side of a switch touches V for a thread on ASID 0, and a thread
//     on a space keeps its registers across one.
//
//   context.ttbr0_follows_next
//     The hardware holds each thread's value after the switch into it, whether
//     or not the switch wrote the register.

#include "test.h"

#include "../../arch/arm64/asid.h"

#include <thylacine/context.h>      // #96: FP_AREA_SIZE, fp_save_area/restore
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
static struct Thread *g_fp_owner;
static struct Thread *g_fp_other;

// Loads V0..V31, then FPSR and FPCR, from a sentinel-shaped buffer. Offsets match
// cpu_switch_context's, Q0+Q1 at +0 through Q30+Q31 at +480.
__attribute__((target("+fp+simd")))
static void fp_load_regs(const u8 *src) {
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
    u64 fpcr = *(const u32 *)&src[32 * 16];
    u64 fpsr = *(const u32 *)&src[32 * 16 + 4];
    __asm__ __volatile__(
        "msr S3_3_C4_C4_1, %0\n"  // FPSR
        "msr S3_3_C4_C4_0, %1\n"  // FPCR
        "isb\n"
        : : "r"(fpsr), "r"(fpcr)
    );
}

__attribute__((target("+fp+simd")))
static void fp_read_regs(u8 *dst) {
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
    u64 fpsr, fpcr;
    __asm__ __volatile__("mrs %0, S3_3_C4_C4_1" : "=r"(fpsr));
    __asm__ __volatile__("mrs %0, S3_3_C4_C4_0" : "=r"(fpcr));
    *(u32 *)&dst[32 * 16]     = (u32)fpcr;
    *(u32 *)&dst[32 * 16 + 4] = (u32)fpsr;
}

// V_k bytes = (k * 17 + j + salt) & 0xff, so a swapped pair or a wrong offset
// misses at a specific byte. FPCR: FZ (bit 24) + DN (bit 25), FPSR: IOC + DZC,
// writable on every ARMv8 implementation (D13.2.91, D13.2.92). Bits 26-29 of FPCR
// are ID-dependent, so the two sentinels differ in FZ alone.
static void fp_build_sentinel(u8 *buf, u8 salt, u32 fpcr, u32 fpsr) {
    for (int k = 0; k < 32; k++) {
        for (int j = 0; j < 16; j++) {
            buf[k * 16 + j] = (u8)(k * 17 + j + salt);
        }
    }
    *(u32 *)&buf[32 * 16]     = fpcr;
    *(u32 *)&buf[32 * 16 + 4] = fpsr;
}

static _Alignas(16) u8 g_fp_clobber[32 * 16 + 8];
static _Alignas(16) u8 g_fp_seen_live[32 * 16 + 8];

static bool fp_bytes_equal(const u8 *a, const u8 *b) {
    for (int i = 0; i < 32 * 16 + 8; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool fp_context_untouched(const struct Thread *t) {
    for (int i = 0; i < 512; i++) {
        if (t->ctx.fp_v[i]) return false;
    }
    return t->ctx.fpcr == 0 && t->ctx.fpsr == 0;
}

// The second thread of a round trip: what it saw on arrival, then a clobber.
static void fp_other_entry(void) {
    for (;;) {
        fp_read_regs(g_fp_seen_live);
        fp_load_regs(g_fp_clobber);
        g_fp_test_state++;
        thread_switch(g_fp_owner);
    }
}

static void fp_owner_entry(void) {
    fp_load_regs(g_fp_sentinel);
    g_fp_test_state++;
    thread_switch(g_fp_other);
    fp_read_regs(g_fp_verify);
    g_fp_test_state++;
    for (;;) thread_switch(g_fp_test_main);
}

// The owner and the other, each on the Proc given. A thread_switch off a fresh
// thread and back leaves both parked, which is how they are freed.
static void fp_round_trip(struct Proc *owner, struct Proc *other, u8 salt) {
    g_fp_test_main = current_thread();
    fp_build_sentinel(g_fp_sentinel, salt, 0x03000000u, 0x00000003u);
    fp_build_sentinel(g_fp_clobber, (u8)(salt + 0x55u), 0x02000000u, 0x000000f0u);
    g_fp_test_state = 0;
    g_fp_owner = thread_create(owner, fp_owner_entry);
    TEST_ASSERT(g_fp_owner != NULL, "thread_create of the owner returned NULL");
    g_fp_other = thread_create(other, fp_other_entry);
    TEST_ASSERT(g_fp_other != NULL, "thread_create of the other returned NULL");
    thread_switch(g_fp_owner);
    TEST_EXPECT_EQ(g_fp_test_state, 3u, "the round trip did not run its three steps");
    thread_free(g_fp_owner);
    thread_free(g_fp_other);
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

void test_fp_round_trip_v_regs_fpsr_fpcr(void) {
    struct Proc *p = proc_alloc();
    struct Proc *q = proc_alloc();
    TEST_ASSERT(p != NULL && q != NULL, "proc_alloc failed");

    fp_round_trip(p, p, 0);
    TEST_ASSERT(fp_bytes_equal(g_fp_verify, g_fp_sentinel),
        "V, FPCR or FPSR did not survive a switch to a thread of the same space");
    TEST_ASSERT(fp_bytes_equal(g_fp_seen_live, g_fp_sentinel) == false,
        "the second thread saw the first's registers: a fresh thread's load did not run");

    fp_round_trip(p, q, 0x21u);
    TEST_ASSERT(fp_bytes_equal(g_fp_verify, g_fp_sentinel),
        "V, FPCR or FPSR did not survive a switch to a thread of another space");

    test_proc_free_drained(q);
    test_proc_free_drained(p);
}

// Two kernel threads. The first's values are still live in the second, and neither
// Context was written.
static void fp_kernel_second(void) {
    fp_read_regs(g_fp_seen_live);
    g_fp_test_state++;
    for (;;) thread_switch(g_fp_owner);
}

static void fp_kernel_first(void) {
    fp_load_regs(g_fp_sentinel);
    g_fp_test_state++;
    thread_switch(g_fp_other);
    fp_read_regs(g_fp_verify);
    g_fp_test_state++;
    for (;;) thread_switch(g_fp_test_main);
}

void test_fp_kernel_thread_holds_no_v(void) {
    g_fp_test_main = current_thread();
    fp_build_sentinel(g_fp_sentinel, 0x33u, 0x03000000u, 0x00000003u);
    g_fp_test_state = 0;
    g_fp_owner = thread_create(kproc(), fp_kernel_first);
    TEST_ASSERT(g_fp_owner != NULL, "thread_create returned NULL");
    g_fp_other = thread_create(kproc(), fp_kernel_second);
    TEST_ASSERT(g_fp_other != NULL, "thread_create returned NULL");
    // Pinned: their V state is this CPU's registers, and a steal after a tick would
    // read another CPU's.
    g_fp_owner->cpu_pinned = true;
    g_fp_other->cpu_pinned = true;
    TEST_ASSERT(fp_context_untouched(g_fp_owner) && fp_context_untouched(g_fp_other),
        "a new kernel thread's Context starts holding FP state");
    thread_switch(g_fp_owner);
    TEST_EXPECT_EQ(g_fp_test_state, 3u, "the two kernel threads did not run");
    TEST_ASSERT(fp_bytes_equal(g_fp_seen_live, g_fp_sentinel),
        "a switch into a kernel thread loaded V, FPCR or FPSR");
    TEST_ASSERT(fp_bytes_equal(g_fp_verify, g_fp_sentinel),
        "a switch back into a kernel thread loaded V, FPCR or FPSR");
    TEST_ASSERT(fp_context_untouched(g_fp_owner),
        "a switch out of a kernel thread saved V, FPCR or FPSR");
    TEST_ASSERT(fp_context_untouched(g_fp_other),
        "a switch out of a kernel thread saved V, FPCR or FPSR (the second)");
    thread_free(g_fp_owner);
    thread_free(g_fp_other);
}

// A thread on a space, a kernel thread that clobbers, and back. The owner's values
// are live on arrival in the kernel thread, and only its Context can bring them back.
void test_fp_user_v_survives_a_kernel_thread(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_fp_test_main = current_thread();
    fp_build_sentinel(g_fp_sentinel, 0x47u, 0x03000000u, 0x00000003u);
    fp_build_sentinel(g_fp_clobber, 0x99u, 0x02000000u, 0x000000f0u);
    g_fp_test_state = 0;
    g_fp_owner = thread_create(p, fp_owner_entry);
    TEST_ASSERT(g_fp_owner != NULL, "thread_create of the owner returned NULL");
    g_fp_other = thread_create(kproc(), fp_other_entry);
    TEST_ASSERT(g_fp_other != NULL, "thread_create of the kernel thread returned NULL");
    g_fp_other->cpu_pinned = true;   // what it reads on arrival is this CPU's registers
    thread_switch(g_fp_owner);
    TEST_EXPECT_EQ(g_fp_test_state, 3u, "the round trip did not run its three steps");
    TEST_ASSERT(fp_bytes_equal(g_fp_seen_live, g_fp_sentinel),
        "a switch into a kernel thread replaced the registers it should have left");
    TEST_ASSERT(fp_bytes_equal(g_fp_verify, g_fp_sentinel),
        "a thread on a space lost V, FPCR or FPSR across a kernel thread");
    TEST_ASSERT(fp_context_untouched(g_fp_other),
        "the kernel thread's switch out saved the clobber");
    thread_free(g_fp_owner);
    thread_free(g_fp_other);
    test_proc_free_drained(p);
}

// A hop for each case the switch decides on. Each thread records what the hardware
// holds and what its own Context says it should, both after the switch into it.
#define TF_HOPS 4u
static struct Thread *g_tf_thread[TF_HOPS];
static u64 g_tf_live[TF_HOPS];
static u64 g_tf_want[TF_HOPS];

static void tf_record(u32 i) {
    u64 v;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(v));
    g_tf_live[i] = v;
    g_tf_want[i] = current_thread()->ctx.ttbr0;
}

static void tf_entry(void *arg) {
    u32 i = (u32)(uintptr_t)arg;
    tf_record(i);
    struct Thread *next = i + 1u < TF_HOPS ? g_tf_thread[i + 1u] : g_fp_test_main;
    for (;;) thread_switch(next);
}

void test_context_ttbr0_follows_next(void) {
    struct Proc *p = proc_alloc();
    struct Proc *q = proc_alloc();
    TEST_ASSERT(p != NULL && q != NULL, "proc_alloc failed");
    g_fp_test_main = current_thread();
    struct Proc *where[TF_HOPS] = { p, p, q, kproc() };
    for (u32 i = 0; i < TF_HOPS; i++) {
        g_tf_live[i] = 0;
        g_tf_thread[i] = thread_create_with_arg(where[i], tf_entry, (void *)(uintptr_t)i);
        TEST_ASSERT(g_tf_thread[i] != NULL, "thread_create_with_arg returned NULL");
    }
    thread_switch(g_tf_thread[0]);
    u64 back;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(back));
    TEST_EXPECT_EQ(g_tf_live[0], g_tf_want[0], "into a space from a kernel thread");
    TEST_EXPECT_EQ(g_tf_live[1], g_tf_want[1], "into a thread of the same space");
    TEST_EXPECT_EQ(g_tf_live[2], g_tf_want[2], "into a thread of another space");
    TEST_EXPECT_EQ(g_tf_live[3], g_tf_want[3], "into a kernel thread from a space");
    TEST_EXPECT_EQ(back, current_thread()->ctx.ttbr0, "back into the test's kernel thread");
    TEST_ASSERT(g_tf_live[0] == g_tf_live[1], "two threads of one space read different values");
    TEST_ASSERT(g_tf_live[1] != g_tf_live[2], "two spaces read one value: the fixture proves nothing");
    TEST_ASSERT((g_tf_live[0] >> ASID_TTBR0_SHIFT) != 0, "a thread on a space runs on ASID 0");
    TEST_EXPECT_EQ(g_tf_live[3] >> ASID_TTBR0_SHIFT, 0ull, "a kernel thread runs on an ASID");
    for (u32 i = 0; i < TF_HOPS; i++) thread_free(g_tf_thread[i]);
    test_proc_free_drained(q);
    test_proc_free_drained(p);
}

// ---------------------------------------------------------------------------
// Task #96: the note-delivery FP save/restore area.
//
// cpu_switch_context's eager save (exercised by fp.round_trip above) covers a
// thread switched OUT. A NOTE HANDLER runs on the SAME thread with no switch,
// so kernel/notes.c must save and restore the interrupted EL0 code's V
// registers explicitly around delivery. This exercises the two helpers that
// do it.
//
// Scope: this proves the MECHANISM (the STP/LDP-Q sequence and the FPSR/FPCR
// tail round-trip correctly). It does NOT prove the PLUMBING -- that notes.c
// actually calls them at both delivery sites and at the restore. The in-guest
// handler legs are what prove that, because a save site can be missing while
// every kernel test still passes.
static _Alignas(16) u8 g_fp_note_area[FP_AREA_SIZE];
static _Alignas(16) u8 g_fp_note_verify[32 * 16];

__attribute__((target("+fp+simd")))
__attribute__((target("+fp+simd")))
void test_fp_note_area_round_trip(void) {
    // A sentinel distinguishable per-register: V<n> gets the byte 0x40 + n,
    // so a swapped pair or an off-by-one offset lands at a diagnosable byte.
    for (int k = 0; k < 32; k++) {
        for (int j = 0; j < 16; j++) g_fp_sentinel[k * 16 + j] = (u8)(0x40 + k);
    }
    u8 *src = g_fp_sentinel;
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "ldp q0,  q1,  [%0, #0]\n"   "ldp q2,  q3,  [%0, #32]\n"
        "ldp q4,  q5,  [%0, #64]\n"  "ldp q6,  q7,  [%0, #96]\n"
        "ldp q8,  q9,  [%0, #128]\n" "ldp q10, q11, [%0, #160]\n"
        "ldp q12, q13, [%0, #192]\n" "ldp q14, q15, [%0, #224]\n"
        "ldp q16, q17, [%0, #256]\n" "ldp q18, q19, [%0, #288]\n"
        "ldp q20, q21, [%0, #320]\n" "ldp q22, q23, [%0, #352]\n"
        "ldp q24, q25, [%0, #384]\n" "ldp q26, q27, [%0, #416]\n"
        "ldp q28, q29, [%0, #448]\n" "ldp q30, q31, [%0, #480]\n"
        : : "r"(src)
        : "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31");

    // The state a note handler would be interrupting.
    fp_save_area(g_fp_note_area);

    // Now be the handler: clobber every V register, the way an autovectorised
    // memcpy or a float operation would.
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "movi v0.16b,  #0xEE\n"  "movi v1.16b,  #0xEE\n"
        "movi v2.16b,  #0xEE\n"  "movi v3.16b,  #0xEE\n"
        "movi v4.16b,  #0xEE\n"  "movi v5.16b,  #0xEE\n"
        "movi v6.16b,  #0xEE\n"  "movi v7.16b,  #0xEE\n"
        "movi v8.16b,  #0xEE\n"  "movi v9.16b,  #0xEE\n"
        "movi v10.16b, #0xEE\n"  "movi v11.16b, #0xEE\n"
        "movi v12.16b, #0xEE\n"  "movi v13.16b, #0xEE\n"
        "movi v14.16b, #0xEE\n"  "movi v15.16b, #0xEE\n"
        "movi v16.16b, #0xEE\n"  "movi v17.16b, #0xEE\n"
        "movi v18.16b, #0xEE\n"  "movi v19.16b, #0xEE\n"
        "movi v20.16b, #0xEE\n"  "movi v21.16b, #0xEE\n"
        "movi v22.16b, #0xEE\n"  "movi v23.16b, #0xEE\n"
        "movi v24.16b, #0xEE\n"  "movi v25.16b, #0xEE\n"
        "movi v26.16b, #0xEE\n"  "movi v27.16b, #0xEE\n"
        "movi v28.16b, #0xEE\n"  "movi v29.16b, #0xEE\n"
        "movi v30.16b, #0xEE\n"  "movi v31.16b, #0xEE\n"
        : :
        : "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31");

    // Name what must be ABSENT and check it FIRST. If the clobber above were
    // optimised away, a pair of no-op helpers would still "pass" the compare
    // below -- the test would be satisfiable by a completely broken system.
    // Verify the handler really did destroy the state before trusting the
    // restore to have brought it back.
    u8 *cdst = g_fp_note_verify;
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "stp q0,  q1,  [%0, #0]\n"   "stp q2,  q3,  [%0, #32]\n"
        "stp q4,  q5,  [%0, #64]\n"  "stp q6,  q7,  [%0, #96]\n"
        "stp q8,  q9,  [%0, #128]\n" "stp q10, q11, [%0, #160]\n"
        "stp q12, q13, [%0, #192]\n" "stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n" "stp q18, q19, [%0, #288]\n"
        "stp q20, q21, [%0, #320]\n" "stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n" "stp q26, q27, [%0, #416]\n"
        "stp q28, q29, [%0, #448]\n" "stp q30, q31, [%0, #480]\n"
        : : "r"(cdst) : "memory");
    TEST_EXPECT_EQ((u32)g_fp_note_verify[0],   0xEEu, "clobber did not take (V0)");
    TEST_EXPECT_EQ((u32)g_fp_note_verify[511], 0xEEu, "clobber did not take (V31)");

    // Return from the handler.
    fp_restore_area(g_fp_note_area);

    u8 *dst = g_fp_note_verify;
    __asm__ __volatile__(
        ".arch_extension fp\n\t"
        "stp q0,  q1,  [%0, #0]\n"   "stp q2,  q3,  [%0, #32]\n"
        "stp q4,  q5,  [%0, #64]\n"  "stp q6,  q7,  [%0, #96]\n"
        "stp q8,  q9,  [%0, #128]\n" "stp q10, q11, [%0, #160]\n"
        "stp q12, q13, [%0, #192]\n" "stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n" "stp q18, q19, [%0, #288]\n"
        "stp q20, q21, [%0, #320]\n" "stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n" "stp q26, q27, [%0, #416]\n"
        "stp q28, q29, [%0, #448]\n" "stp q30, q31, [%0, #480]\n"
        : : "r"(dst) : "memory");

    for (int i = 0; i < 32 * 16; i++) {
        if (g_fp_note_verify[i] != g_fp_sentinel[i]) {
            TEST_EXPECT_EQ((u32)g_fp_note_verify[i], (u32)g_fp_sentinel[i],
                "fp_restore_area did not restore V regs (byte index = i/16 -> V reg)");
            return;
        }
    }
}
