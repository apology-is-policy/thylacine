// P6-pouch-mem: SYS_BURROW_ATTACH / SYS_BURROW_DETACH tests.
//
// Drives the _for_proc inners (the non-static cores of the SVC handlers,
// defined in kernel/syscall.c) on a fresh proc_alloc'd Proc:
//
//   sys_burrow.attach_returns_window_va
//     attach installs an anonymous RW VMA in the burrow-attach window;
//     the backing Burrow has mapping_count 1, handle_count 0 (Tier 1).
//   sys_burrow.attach_detach_round_trip
//     attach then detach removes the VMA and frees the Burrow; the freed
//     range is reused by the next attach.
//   sys_burrow.attach_distinct
//     repeated attaches yield distinct, non-overlapping, in-window VMAs.
//   sys_burrow.attach_rounds_up
//     a sub-page-multiple request is rounded up to a whole-page span.
//   sys_burrow.attach_rejects_bad_length
//     length 0 / length > BURROW_ATTACH_MAX / NULL Proc → -1.
//   sys_burrow.detach_rejects
//     wrong base / wrong length / unaligned base / zero length /
//     double-detach → -1, and the VMA survives every rejected detach.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/exec.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_sys_burrow_attach_returns_window_va(void);
void test_sys_burrow_attach_detach_round_trip(void);
void test_sys_burrow_attach_distinct(void);
void test_sys_burrow_attach_rounds_up(void);
void test_sys_burrow_attach_rejects_bad_length(void);
void test_sys_burrow_detach_rejects(void);
void test_sys_burrow_detach_window_confined(void);

// The non-static inners of the SVC handlers (defined in kernel/syscall.c).
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw,
                                      u64 length_raw);

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;             // PROC_STATE_ZOMBIE; proc_free drains VMAs
    proc_free(p);
}

// True iff `va` is inside the burrow-attach window.
static bool in_window(u64 va) {
    return va >= EXEC_USER_BURROW_BASE && va < EXEC_USER_BURROW_TOP;
}

void test_sys_burrow_attach_returns_window_va(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "attach returned a non-positive result");
    u64 va = (u64)r;
    TEST_ASSERT(in_window(va), "attach VA outside the burrow window");
    TEST_ASSERT((va & (PAGE_SIZE - 1)) == 0, "attach VA not page-aligned");

    struct Vma *vma = vma_lookup(p, va);
    TEST_ASSERT(vma != NULL, "no VMA at the attached VA");
    TEST_EXPECT_EQ(vma->vaddr_start, va,             "VMA start == attach VA");
    TEST_EXPECT_EQ(vma->vaddr_end,   va + PAGE_SIZE, "VMA spans one page");
    TEST_EXPECT_EQ(vma->prot,        VMA_PROT_RW,    "VMA prot is RW");

    // Tier 1: the VMA owns the Burrow — mapping_count 1, handle_count 0.
    struct Burrow *b = vma->burrow;
    TEST_ASSERT(b != NULL, "VMA has no backing Burrow");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "Burrow mapping_count == 1");
    TEST_EXPECT_EQ(burrow_handle_count(b),  0, "Burrow handle_count == 0");

    drop_proc(p);
}

void test_sys_burrow_attach_detach_round_trip(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    u64 destroyed_before = burrow_total_destroyed();

    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "attach failed");
    u64 va = (u64)r;

    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, PAGE_SIZE), 0,
        "detach of an attached region succeeds");
    TEST_ASSERT(vma_lookup(p, va) == NULL, "VMA gone after detach");
    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, 1ull,
        "detach freed exactly one Burrow");

    // The detached range is free again — the next attach reuses it.
    s64 r2 = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r2 > 0 && (u64)r2 == va, "re-attach reuses the detached gap");

    drop_proc(p);
}

void test_sys_burrow_attach_distinct(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    s64 a = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    s64 b = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    s64 c = sys_burrow_attach_for_proc(p, 2 * PAGE_SIZE);
    TEST_ASSERT(a > 0 && b > 0 && c > 0, "three attaches succeed");
    TEST_ASSERT((u64)a != (u64)b && (u64)b != (u64)c && (u64)a != (u64)c,
        "attaches return distinct VAs");
    TEST_ASSERT(in_window((u64)a) && in_window((u64)b) && in_window((u64)c),
        "every attach VA is inside the window");

    // Each VA resolves to its own VMA — the three regions don't overlap.
    struct Vma *va = vma_lookup(p, (u64)a);
    struct Vma *vb = vma_lookup(p, (u64)b);
    struct Vma *vc = vma_lookup(p, (u64)c);
    TEST_ASSERT(va && vb && vc, "each attach VA resolves to a VMA");
    TEST_ASSERT(va != vb && vb != vc && va != vc, "three distinct VMAs");

    drop_proc(p);
}

void test_sys_burrow_attach_rounds_up(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // A request of one page + 1 byte rounds up to a two-page span.
    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE + 1);
    TEST_ASSERT(r > 0, "attach of PAGE_SIZE+1 failed");
    u64 va = (u64)r;

    struct Vma *vma = vma_lookup(p, va);
    TEST_ASSERT(vma != NULL, "no VMA at the attached VA");
    TEST_EXPECT_EQ(vma->vaddr_end - vma->vaddr_start, 2ull * PAGE_SIZE,
        "PAGE_SIZE+1 request rounded up to a 2-page VMA");

    // Detach accepts the original (un-rounded) length — it rounds the
    // same way the attach did, so the spans match.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, PAGE_SIZE + 1), 0,
        "detach with the un-rounded length matches the rounded span");
    TEST_ASSERT(vma_lookup(p, va) == NULL, "VMA gone after detach");

    // An exact page-multiple request is not rounded — the span matches.
    s64 r3 = sys_burrow_attach_for_proc(p, 3 * PAGE_SIZE);
    TEST_ASSERT(r3 > 0, "attach of an exact 3-page multiple failed");
    struct Vma *v3 = vma_lookup(p, (u64)r3);
    TEST_ASSERT(v3 != NULL, "no VMA at the 3-page attach");
    TEST_EXPECT_EQ(v3->vaddr_end - v3->vaddr_start, 3ull * PAGE_SIZE,
        "exact 3-page request spans exactly 3 pages");

    drop_proc(p);
}

void test_sys_burrow_attach_rejects_bad_length(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    TEST_EXPECT_EQ(sys_burrow_attach_for_proc(p, 0), -1L,
        "attach of length 0 rejected");
    TEST_EXPECT_EQ(sys_burrow_attach_for_proc(p, (u64)BURROW_ATTACH_MAX + 1),
                   -1L, "attach above BURROW_ATTACH_MAX rejected");
    TEST_EXPECT_EQ(sys_burrow_attach_for_proc(NULL, PAGE_SIZE), -1L,
        "attach with a NULL Proc rejected");

    // No rejected call installed a VMA.
    TEST_ASSERT(p->vmas == NULL, "rejected attach must not install a VMA");

    drop_proc(p);
}

void test_sys_burrow_detach_rejects(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    s64 r = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "attach failed");
    u64 va = (u64)r;

    // Wrong base / wrong length / unaligned base / zero length → -1, and
    // the VMA survives every rejected detach.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va + PAGE_SIZE, PAGE_SIZE),
                   -1L, "detach at the wrong base rejected");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, 2 * PAGE_SIZE),
                   -1L, "detach with the wrong length rejected");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va + 1, PAGE_SIZE),
                   -1L, "detach with an unaligned base rejected");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, 0),
                   -1L, "detach with zero length rejected");
    TEST_ASSERT(vma_lookup(p, va) != NULL, "VMA survives the rejected detaches");

    // The matching detach succeeds; a second (double) detach fails.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, PAGE_SIZE), 0,
        "the matching detach succeeds");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, PAGE_SIZE), -1L,
        "double-detach rejected");

    drop_proc(p);
}

// F1 regression (P6-pouch-mem-a audit): SYS_BURROW_DETACH must refuse a
// vaddr outside the burrow-attach window, so a caller cannot pass the
// coordinates of its own ELF-segment / stack / guard VMA and have
// burrow_unmap dismantle it. Pre-fix this detach succeeded.
void test_sys_burrow_detach_window_confined(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *burrow = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(burrow != NULL, "burrow_create_anon failed");

    // Install a VMA BELOW the burrow window — the shape of an ELF
    // segment or the stack (vaddr < EXEC_USER_BURROW_BASE).
    u64 outside = 0x10000000ull;     // 256 MiB — well below the window
    TEST_EXPECT_EQ(burrow_map(p, burrow, outside, PAGE_SIZE, VMA_PROT_RW), 0,
        "install an out-of-window VMA");

    // Detach must reject it — and the VMA must survive untouched.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, outside, PAGE_SIZE), -1L,
        "detach of a sub-window VMA rejected");
    TEST_ASSERT(vma_lookup(p, outside) != NULL,
        "the out-of-window VMA survives the rejected detach");

    vma_drain(p);
    p->state = 2;                    // PROC_STATE_ZOMBIE
    proc_free(p);
    burrow_unref(burrow);
}
