// HX-1: Halls of Extinction crash-dump unit tests.
//
// The dump itself (register/backtrace/hexdump UART emission) is exercised
// behaviorally -- it runs on the real fault path and is read off the UART
// log. These unit tests cover the pure decision logic that keeps the dump
// SOUND under a corrupt machine: the fp-chain sanity gate (HX-I2), the
// KASLR link-address translation, and the per-CPU live-frame save/restore
// discipline the exception wrappers rely on.

#include "test.h"

#include "../../arch/arm64/halls.h"

#include <thylacine/types.h>

void test_halls_fp_sane_accepts_valid(void);
void test_halls_fp_sane_rejects_misaligned(void);
void test_halls_fp_sane_rejects_non_increasing(void);
void test_halls_fp_sane_rejects_out_of_range(void);
void test_halls_link_addr_removes_slide(void);
void test_halls_link_addr_underflow_guarded(void);
void test_halls_frame_enter_leave_nesting(void);
void test_halls_frame_is_live_gate(void);

// A plausible kernel-stack window for the gate tests.
#define LO  0xffff000000010000ull
#define HI  (LO + 0x10000ull)

void test_halls_fp_sane_accepts_valid(void) {
    TEST_ASSERT(halls_fp_is_sane(LO + 0x100, LO, LO, HI),
                "16-aligned, > prev, in [lo,hi)");
    TEST_ASSERT(halls_fp_is_sane(LO, 0, LO, HI),
                "first frame: prev=0, fp at lo");
    TEST_ASSERT(halls_fp_is_sane(HI - 0x10, LO + 0x200, LO, HI),
                "near top, still below hi");
}

void test_halls_fp_sane_rejects_misaligned(void) {
    TEST_ASSERT(!halls_fp_is_sane(LO + 0x108, LO, LO, HI), "8-misaligned");
    TEST_ASSERT(!halls_fp_is_sane(LO + 0x104, LO, LO, HI), "4-misaligned");
    TEST_ASSERT(!halls_fp_is_sane(LO + 0x101, LO, LO, HI), "1-misaligned");
}

void test_halls_fp_sane_rejects_non_increasing(void) {
    // Strictly increasing is the cycle-freedom guarantee (HX-I2).
    TEST_ASSERT(!halls_fp_is_sane(LO + 0x100, LO + 0x100, LO, HI), "== prev");
    TEST_ASSERT(!halls_fp_is_sane(LO + 0x100, LO + 0x200, LO, HI), "< prev");
}

void test_halls_fp_sane_rejects_out_of_range(void) {
    TEST_ASSERT(!halls_fp_is_sane(LO - 0x10, 0, LO, HI), "below lo");
    TEST_ASSERT(!halls_fp_is_sane(HI, LO, LO, HI), "at hi (exclusive)");
    TEST_ASSERT(!halls_fp_is_sane(HI + 0x10, LO, LO, HI), "above hi");
}

void test_halls_link_addr_removes_slide(void) {
    // A slid code address (>= offset) maps back to its link-time VA.
    TEST_EXPECT_EQ(halls_link_addr(0x1000ull + 0x40000000ull, 0x40000000ull),
                   0x1000ull, "addr - offset");
    TEST_EXPECT_EQ(halls_link_addr(0x40000000ull, 0x40000000ull),
                   0x0ull, "addr == offset -> 0");
}

void test_halls_link_addr_underflow_guarded(void) {
    // A value below the offset is not a slid code address -- left untouched,
    // never wrapped around to a huge bogus link addr.
    TEST_EXPECT_EQ(halls_link_addr(0x100ull, 0x40000000ull),
                   0x100ull, "below offset -> unchanged");
    TEST_EXPECT_EQ(halls_link_addr(0xDEADBEEFull, 0ull),
                   0xDEADBEEFull, "zero offset -> unchanged");
}

void test_halls_frame_enter_leave_nesting(void) {
    // halls_enter/leave only store/return the pointer -- never dereference --
    // so dummy non-NULL frames are safe stand-ins.
    struct exception_context *a = (struct exception_context *)0x100;
    struct exception_context *b = (struct exception_context *)0x200;

    // Mask IRQs across the global-slot manipulation so a timer-tick IRQ's
    // own enter/leave can't transiently perturb the per-CPU slot between a
    // mutation and its observation (no-flaky-tests discipline). Snapshot all
    // observations into locals, restore DAIF, THEN assert -- so a failing
    // TEST_ASSERT never early-returns with IRQs still masked.
    u64 daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    __asm__ __volatile__("msr daifset, #2" ::: "memory");

    const struct exception_context *orig        = halls_current_frame();
    const struct exception_context *pa          = halls_enter_frame(a);
    const struct exception_context *cur_a        = halls_current_frame();
    const struct exception_context *pb          = halls_enter_frame(b);
    const struct exception_context *cur_b        = halls_current_frame();
    halls_leave_frame(pb);
    const struct exception_context *cur_after_b  = halls_current_frame();
    halls_leave_frame(pa);
    const struct exception_context *cur_after_a  = halls_current_frame();

    __asm__ __volatile__("msr daif, %0" :: "r"(daif) : "memory");

    TEST_ASSERT(pa == orig,         "enter(a) returns prior slot");
    TEST_ASSERT(cur_a == a,         "slot == a after enter(a)");
    TEST_ASSERT(pb == a,            "enter(b) returns a");
    TEST_ASSERT(cur_b == b,         "slot == b after enter(b)");
    TEST_ASSERT(cur_after_b == a,   "leave(b) restores a");
    TEST_ASSERT(cur_after_a == orig,"leave(a) restores original");
}

// F1 regression: the HX-I4 plausibility gate. The pre-fix code trusted the
// per-CPU slot unconditionally; a slot stranded by a since-exited Proc (its
// kstack freed + recycled) pointed at a freed/other-thread stack -- far from
// the dumper's SP -- yet was dumped, fabricating a "source EL0" record AND
// suppressing the correct capture-current. The gate rejects a frame that is
// below cur_sp or more than one stack (16 KiB) above it.
void test_halls_frame_is_live_gate(void) {
    u64 sp = 0xffffa00012340000ull;
    TEST_ASSERT(halls_frame_is_live(sp + 0x200, sp), "frame just above sp -> live");
    TEST_ASSERT(halls_frame_is_live(sp, sp),         "frame == sp -> live (edge)");
    TEST_ASSERT(halls_frame_is_live(sp + 16u * 1024u, sp),
                "frame at the 16 KiB slack boundary -> live");
    // Stale / dangling: below sp (lower stack) or beyond one stack above it.
    TEST_ASSERT(!halls_frame_is_live(sp - 0x10, sp),       "below sp -> stale");
    TEST_ASSERT(!halls_frame_is_live(sp + 16u * 1024u + 16u, sp),
                "just past the slack boundary -> stale");
    TEST_ASSERT(!halls_frame_is_live(sp + 0x100000, sp),   "1 MiB above (other stack) -> stale");
}
