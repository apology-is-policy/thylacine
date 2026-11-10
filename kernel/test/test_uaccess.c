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

#include "../../arch/arm64/exception.h"
#include "../../arch/arm64/mmu.h"
#include "../../arch/arm64/uaccess.h"
#include "../../arch/arm64/uart.h"
#include "../../mm/phys.h"

#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

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
    // against the supplied VA — translation fault at L2, FAR = the
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

// The vector calls a frame an overflow only inside the stack range (bit 44) and in a
// slot's guard half (bit 14 clear). Any other stack has bit 14 as it falls, as the boot
// and idle stacks do, and a fault taken on one is an ordinary fault.
static volatile s64 g_tkf_rc;
static volatile u64 g_tkf_frame;
static u8           g_tkf_out;

static void tkf_body(void) {
    g_tkf_frame = (u64)(uintptr_t)__builtin_frame_address(0);
    g_tkf_rc = uaccess_load_u8(0x10000000ull, &g_tkf_out);
}

void test_thread_kstack_fault_off_the_range_is_no_overflow(void) {
    // Order 4 starts on a 64 KiB boundary, so its first 16 KiB have bit 14 clear.
    struct page *pg = alloc_pages(4, 0);
    TEST_ASSERT(pg != NULL, "alloc_pages");
    u64 base = (u64)(uintptr_t)pa_to_kva(page_to_pa(pg));
    u64 sp = base + 16u * 1024u - 16u;
    void (*fn)(void) = tkf_body;
    g_tkf_rc = 0;
    g_tkf_frame = 0;
    __asm__ __volatile__("mov x19, sp\n"
                         "mov sp, %0\n"
                         "blr %1\n"
                         "mov sp, x19\n"
                         :
                         : "r" (sp), "r" (fn)
                         : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
                           "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x30",
                           "memory", "cc");
    u64 frame = g_tkf_frame;
    free_pages(pg, 4);
    TEST_ASSERT(frame > base && frame < sp, "CONTROL: the body ran on the stack it was given");
    TEST_ASSERT((frame & (1ull << KSTACK_RANGE_TAG_BIT)) == 0, "CONTROL: that stack is off the range");
    TEST_ASSERT((frame & (1ull << KSTACK_USABLE_BIT)) == 0, "CONTROL: and has bit 14 clear");
    TEST_EXPECT_EQ(g_tkf_rc, (s64)-1, "the fault was taken and fixed up as any other");
}

// CF-3 A: the bulk copy primitives' negative path. kproc's TTBR0 has no
// user-half mappings, so every user-VA touch faults; the (alignment, len)
// variants steer the FIRST faulting instruction to each of the three fault
// points (head strb / body str / tail strb for copy_out; the ldrb/ldr
// mirrors for copy_in), proving each fixup entry resolves. The positive
// path + mid-copy semantics are exercised in-guest by joey's boot-fatal
// bulk-I/O probe (every EL0 read/write above SYS_RW_STACK rides them).

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

// A device window whose device refuses the access: the copy fails, and the
// kernel does not end. The child maps a physical address the machine has
// nothing at into its own address space and hands that address to every
// uaccess arm. Where the machine reports the refusal (a synchronous external
// abort, as TCG does) each call returns -1; where the hypervisor completes the
// access itself each returns 0. Either way all seven agree.
#define UB_VA      0x30000000ull     // 768 MiB, user half; nothing else maps it
#define UB_PA      0x0b000000ull     // a hole in the virt machine's low device region: no RAM, no device
#define UB_CALLS   7
static volatile s64 g_ub_rc[UB_CALLS];
static volatile int g_ub_map_rc, g_ub_under_rc, g_ub_past_rc;

static void ub_child(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_ub_map_rc = mmu_install_user_pte(p->as, proc_resource_exempt(p), UB_VA, (paddr_t)UB_PA, VMA_PROT_RW, true);
    if (g_ub_map_rc != 0) exits("map");
    // A leaf may not name an address past the CPU's physical address size: its
    // first use would be an address size fault, which no fixup answers. Neither
    // of these two is ever touched.
    paddr_t top = (paddr_t)1 << mmu_pa_bits();
    g_ub_under_rc = mmu_install_user_pte(p->as, proc_resource_exempt(p), UB_VA + PAGE_SIZE,     top - PAGE_SIZE, VMA_PROT_RW, true);
    g_ub_past_rc  = mmu_install_user_pte(p->as, proc_resource_exempt(p), UB_VA + 2 * PAGE_SIZE, top,             VMA_PROT_RW, true);
    u8 buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0;
    u8 one = 0;
    g_ub_rc[0] = uaccess_load_u8(UB_VA, &one);
    g_ub_rc[1] = uaccess_copy_in(buf, UB_VA + 1, 16);    // head arm
    g_ub_rc[2] = uaccess_copy_in(buf, UB_VA, 64);        // body arm
    g_ub_rc[3] = uaccess_copy_in(buf, UB_VA, 3);         // tail arm
    g_ub_rc[4] = uaccess_copy_out(UB_VA + 1, buf, 16);
    g_ub_rc[5] = uaccess_copy_out(UB_VA, buf, 64);
    g_ub_rc[6] = uaccess_copy_out(UB_VA, buf, 3);
    exits("ok");
}

void test_uaccess_bus_error_fails_the_copy(void) {
    TEST_ASSERT(!phys_pa_is_ram((paddr_t)UB_PA), "premise: the probe address is not RAM");
    for (int i = 0; i < UB_CALLS; i++) g_ub_rc[i] = -99;
    g_ub_map_rc = g_ub_under_rc = g_ub_past_rc = -99;

    int pid = rfork(RFPROC, ub_child, NULL);
    TEST_ASSERT(pid > 0, "rfork");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid");
    TEST_EXPECT_EQ(g_ub_map_rc, 0, "the child mapped the device leaf");
    TEST_EXPECT_EQ(status, 0, "the child ran every copy and exited cleanly");
    TEST_EXPECT_EQ(g_ub_under_rc, 0, "control: a leaf at the last page under the physical address size installs");
    TEST_EXPECT_EQ(g_ub_past_rc, -1, "a leaf at the first page past the physical address size is refused");

    int refused = 0;
    for (int i = 0; i < UB_CALLS; i++) {
        s64 rc = g_ub_rc[i];
        TEST_ASSERT(rc == 0 || rc == -1, "a copy at a device window returns 0 or -1");
        if (rc == -1) refused++;
    }
    TEST_ASSERT(refused == 0 || refused == UB_CALLS, "every arm met the same answer from the machine");
    uart_puts("    uaccess.bus_error: the machine refused ");
    uart_putdec((u64)refused);
    uart_puts(" of 7 copies\n");
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
