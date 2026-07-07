// R12-uaccess unit tests.
//
// Three concerns:
//   1. Fixup table is well-formed at link time (start < end; whole
//      number of (op_pc, fixup_pc) pairs; at least one entry exists).
//   2. uaccess_fixup_lookup returns the fixup PC for the known op PC
//      AND returns 0 for an arbitrary kernel PC not in the table.
//   3. uaccess_load_u8 on an unmapped user VA returns -1 (negative
//      path; kernel tests run inside kproc whose TTBR0 = l0_ttbr0
//      has no user-half mappings). The positive path — load_u8 on
//      a mapped user VA — is exercised by the userspace integration
//      tests (virtio-blk-rw, virtio-net-arp, virtio-net-loop) which
//      call SYS_PUTS from EL0 after R12-uaccess removed their
//      pretouch_rodata_pages() workarounds; if the positive path
//      regressed, every such test would fail at first puts.

#include "test.h"

#include "../../arch/arm64/uaccess.h"

#include <thylacine/types.h>

void test_uaccess_fixup_table_well_formed(void);
void test_uaccess_fixup_lookup_known(void);
void test_uaccess_fixup_lookup_unknown_returns_zero(void);
void test_uaccess_load_u8_unmapped_user_va_returns_minus1(void);
void test_uaccess_copy_out_unmapped_faults_all_arms(void);
void test_uaccess_copy_in_unmapped_faults_all_arms(void);
void test_uaccess_copy_fixup_entries_present(void);

// Each fixup table entry — must mirror arch/arm64/uaccess.c's
// struct layout. PC-relative s32 offsets; absolute PC is recovered
// by adding the offset to the address of the field itself.
struct uaccess_fixup_entry {
    s32 op_rel;
    s32 fixup_rel;
};

// Linker bounds of .uaccess_fixup region (kernel.ld emits these).
extern const struct uaccess_fixup_entry _uaccess_fixup_start[];
extern const struct uaccess_fixup_entry _uaccess_fixup_end[];

// uaccess.S exports these labels for the test only — they're the
// faulting-load and fault-recovery points of uaccess_load_u8. Used to
// cross-check uaccess_fixup_lookup against the actual primitive.
extern char uaccess_load_u8_op[];
extern char uaccess_load_u8_fault[];

void test_uaccess_fixup_table_well_formed(void) {
    // start < end (table is non-empty).
    TEST_ASSERT(_uaccess_fixup_start < _uaccess_fixup_end,
        "uaccess fixup table must be non-empty");

    // The byte span is a whole multiple of entries (8 B per entry).
    u64 bytes = (u64)((const char *)_uaccess_fixup_end -
                      (const char *)_uaccess_fixup_start);
    TEST_EXPECT_EQ(bytes % sizeof(struct uaccess_fixup_entry), 0ull,
        "fixup table size must be a multiple of entry size");
    TEST_ASSERT(bytes >= sizeof(struct uaccess_fixup_entry),
        "fixup table must hold at least one entry");

    // Each entry's resolved op_pc and fixup_pc lie inside the kernel
    // image's TEXT high-VA range. Cheap acceptance: both must be
    // non-NULL after the PC-relative add.
    const struct uaccess_fixup_entry *e = _uaccess_fixup_start;
    while (e < _uaccess_fixup_end) {
        u64 op_pc =
            (u64)(uintptr_t)&e->op_rel + (u64)(s64)e->op_rel;
        u64 fixup_pc =
            (u64)(uintptr_t)&e->fixup_rel + (u64)(s64)e->fixup_rel;
        TEST_ASSERT(op_pc != 0, "fixup table op_pc must be non-zero");
        TEST_ASSERT(fixup_pc != 0, "fixup table fixup_pc must be non-zero");
        e++;
    }
}

void test_uaccess_fixup_lookup_known(void) {
    // The uaccess_load_u8 primitive emits a (op_pc, fixup_pc) =
    // (uaccess_load_u8_op, uaccess_load_u8_fault) entry. Lookup with
    // the op_pc must return the fault_pc verbatim — the path the
    // dispatcher consults to install ctx->elr.
    u64 op    = (u64)(uintptr_t)uaccess_load_u8_op;
    u64 fault = (u64)(uintptr_t)uaccess_load_u8_fault;

    u64 got = uaccess_fixup_lookup(op);
    TEST_EXPECT_EQ(got, fault,
        "lookup(load_u8_op) must return load_u8_fault");
}

void test_uaccess_fixup_lookup_unknown_returns_zero(void) {
    // Arbitrary kernel PC not in the fixup table (this very function's
    // address). The lookup must miss and return 0 so the dispatcher
    // falls through to the existing kernel-fault extinction path —
    // a genuine kernel-pointer corruption must NOT be silently
    // swallowed by the uaccess machinery.
    u64 not_in_table =
        (u64)(uintptr_t)&test_uaccess_fixup_lookup_unknown_returns_zero;
    TEST_EXPECT_EQ(uaccess_fixup_lookup(not_in_table), 0ull,
        "lookup of non-uaccess PC must return 0");

    // A NULL PC (degenerate input) also misses cleanly.
    TEST_EXPECT_EQ(uaccess_fixup_lookup(0), 0ull,
        "lookup of NULL PC must return 0");
}

void test_uaccess_load_u8_unmapped_user_va_returns_minus1(void) {
    // The test runs in kproc context; kproc's TTBR0 = l0_ttbr0 has
    // an empty user half post-P3-Bda. uaccess_load_u8 issues an ldrb
    // against the supplied VA — translation fault at L0, FAR = the
    // VA, ELR = uaccess_load_u8_op. exception_sync_curr_el detects
    // FAR < USER_VA_TOP + fixup table hit, calls userland_demand_page
    // (which fails because kproc has no VMA covering 0x10000000),
    // sets ctx->elr := uaccess_load_u8_fault, ERETs. The fault label
    // returns -1.
    //
    // This is the regression test for the fault-fixup path. Without
    // the dispatcher logic, the load would fall through to the
    // "unhandled kernel translation fault" extinction and the kernel
    // would halt.
    u8 out = 0xAB;        // poisoned; expected unchanged on fault.
    s64 rc = uaccess_load_u8(0x10000000ull, &out);
    TEST_EXPECT_EQ(rc, (s64)-1,
        "uaccess_load_u8 on unmapped user VA must return -1");
    TEST_EXPECT_EQ((u64)out, 0xABull,
        "uaccess_load_u8 must not write *out on fault");

    // A second unmapped VA, well separated from the first. Verifies
    // the dispatcher doesn't latch on a particular FAR.
    rc = uaccess_load_u8(0x40000000ull, &out);
    TEST_EXPECT_EQ(rc, (s64)-1,
        "uaccess_load_u8 on a second unmapped VA must also return -1");
}

// CF-3 A: the bulk copy primitives' negative path. kproc's TTBR0 has no
// user-half mappings, so every user-VA touch faults; the (alignment, len)
// variants steer the FIRST faulting instruction to each of the three fault
// points (head strb / body str / tail strb for copy_out; the ldrb/ldr
// mirrors for copy_in), proving each fixup entry resolves. The positive
// path + mid-copy semantics are exercised in-guest by joey's boot-fatal
// bulk-I/O probe (every EL0 read/write above SYS_RW_STACK rides them).

extern char uaccess_copy_out_op_head[];
extern char uaccess_copy_out_op_body[];
extern char uaccess_copy_out_op_tail[];
extern char uaccess_copy_out_fault[];
extern char uaccess_copy_in_op_head[];
extern char uaccess_copy_in_op_body[];
extern char uaccess_copy_in_op_tail[];
extern char uaccess_copy_in_fault[];

void test_uaccess_copy_out_unmapped_faults_all_arms(void) {
    static const u8 src[64] = { 1, 2, 3 };

    // len == 0: no user deref at all -- must succeed.
    TEST_EXPECT_EQ(uaccess_copy_out(0x10000000ull, src, 0), (s64)0,
        "copy_out len=0 must succeed without touching the VA");

    // Unaligned dst -> the HEAD strb is the first fault point.
    TEST_EXPECT_EQ(uaccess_copy_out(0x10000001ull, src, 16), (s64)-1,
        "copy_out head-arm fault must return -1");

    // Aligned dst, len >= 8 -> the BODY str is the first fault point.
    TEST_EXPECT_EQ(uaccess_copy_out(0x10000000ull, src, 64), (s64)-1,
        "copy_out body-arm fault must return -1");

    // Aligned dst, len < 8 -> the TAIL strb is the first fault point.
    TEST_EXPECT_EQ(uaccess_copy_out(0x10000000ull, src, 3), (s64)-1,
        "copy_out tail-arm fault must return -1");
}

void test_uaccess_copy_in_unmapped_faults_all_arms(void) {
    u8 dst[64];
    dst[0] = 0xAB;   // poisoned; the head fault must not have stored.

    TEST_EXPECT_EQ(uaccess_copy_in(dst, 0x10000000ull, 0), (s64)0,
        "copy_in len=0 must succeed without touching the VA");

    TEST_EXPECT_EQ(uaccess_copy_in(dst, 0x10000001ull, 16), (s64)-1,
        "copy_in head-arm fault must return -1");
    TEST_EXPECT_EQ((u64)dst[0], 0xABull,
        "copy_in must not write dst[0] when the first load faults");

    TEST_EXPECT_EQ(uaccess_copy_in(dst, 0x10000000ull, 64), (s64)-1,
        "copy_in body-arm fault must return -1");

    TEST_EXPECT_EQ(uaccess_copy_in(dst, 0x10000000ull, 3), (s64)-1,
        "copy_in tail-arm fault must return -1");
}

void test_uaccess_copy_fixup_entries_present(void) {
    // Each of the six faulting instructions must resolve to its shared
    // fault label -- the lookup path the dispatcher takes to install
    // ctx->elr on a copy fault.
    TEST_EXPECT_EQ(uaccess_fixup_lookup((u64)(uintptr_t)uaccess_copy_out_op_head),
        (u64)(uintptr_t)uaccess_copy_out_fault, "copy_out head fixup");
    TEST_EXPECT_EQ(uaccess_fixup_lookup((u64)(uintptr_t)uaccess_copy_out_op_body),
        (u64)(uintptr_t)uaccess_copy_out_fault, "copy_out body fixup");
    TEST_EXPECT_EQ(uaccess_fixup_lookup((u64)(uintptr_t)uaccess_copy_out_op_tail),
        (u64)(uintptr_t)uaccess_copy_out_fault, "copy_out tail fixup");
    TEST_EXPECT_EQ(uaccess_fixup_lookup((u64)(uintptr_t)uaccess_copy_in_op_head),
        (u64)(uintptr_t)uaccess_copy_in_fault, "copy_in head fixup");
    TEST_EXPECT_EQ(uaccess_fixup_lookup((u64)(uintptr_t)uaccess_copy_in_op_body),
        (u64)(uintptr_t)uaccess_copy_in_fault, "copy_in body fixup");
    TEST_EXPECT_EQ(uaccess_fixup_lookup((u64)(uintptr_t)uaccess_copy_in_op_tail),
        (u64)(uintptr_t)uaccess_copy_in_fault, "copy_in tail fixup");
}
