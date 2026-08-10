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
#include <thylacine/dev.h>          // D-3c F1: a stub Dev with a close hook
#include <thylacine/exec.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>        // D-3c F1: spoor_alloc for a FILE burrow
#include <thylacine/syscall.h>
#include <thylacine/thread.h>       // D-3c F1: current_thread()->preempt_count
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_sys_burrow_attach_returns_window_va(void);
void test_sys_burrow_attach_detach_round_trip(void);
void test_sys_burrow_attach_distinct(void);
void test_sys_burrow_attach_rounds_up(void);
void test_sys_burrow_attach_rejects_bad_length(void);
void test_sys_burrow_detach_rejects(void);
void test_sys_burrow_detach_window_confined(void);
void test_sys_burrow_attach_lazy_window_va(void);
void test_sys_burrow_lazy_len_from_args(void);

// The non-static inners of the SVC handlers (defined in kernel/syscall.c).
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw,
                                      u64 length_raw);
// Overcommit / I-32 (ARCH section 6.5): the demand-zero lazy attach.
extern s64 sys_burrow_attach_lazy_for_proc(struct Proc *p, u64 length_raw);
// CL-4: the pure dual-ABI length selector behind the dispatch case.
extern u64 burrow_lazy_len_from_args(u64 x0, u64 x1, u64 flags, u64 fd_raw);

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;             // PROC_STATE_ZOMBIE; proc_free drains VMAs
    proc_free(p);
}

// DISTRO D-3b: burrow_map_fixed under the lock its contract requires.
static int map_fixed_locked(struct Proc *p, struct Burrow *b, u64 va,
                            size_t len, u32 prot, u64 off) {
    spin_lock(&p->as->lock);
    int rc = burrow_map_fixed(p, b, va, len, prot, off);
    spin_unlock(&p->as->lock);
    return rc;
}

// A fresh lazy-anon Burrow with ONE ref for the caller.
static struct Burrow *fresh_anon(size_t len) {
    return burrow_create_anon_lazy(len);
}

// The property the whole split rests on: for every VA a surviving VMA still
// covers, the byte it names is unchanged. `burrow_offset + (va - vaddr_start)`
// IS that byte's identity -- it is exactly the expression the demand-page arms
// (arch/arm64/fault.c) and the #190 post-sleep geometry check compute.
static u64 byte_identity(struct Vma *v, u64 va) {
    return v->burrow_offset + (va - v->vaddr_start);
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
    TEST_ASSERT(p->as->vmas == NULL, "rejected attach must not install a VMA");

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

// Overcommit / I-32 (ARCH section 6.5): a lazy attach reserves a window VA + an
// ANON_LAZY VMA but commits NO pages (page_count stays 0 -- the free reservation);
// the I-32 VMA-count axis IS charged. The fault-side demand-zero behavior is covered
// in test_demand_page (lazy_zero_fill / decommit_refault / charge_on_fault_oom).
void test_sys_burrow_attach_lazy_window_va(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    s64 r = sys_burrow_attach_lazy_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(r > 0, "lazy attach returned a non-positive result");
    u64 va = (u64)r;
    TEST_ASSERT(in_window(va), "lazy attach VA outside the burrow window");
    TEST_ASSERT((va & (PAGE_SIZE - 1)) == 0, "lazy attach VA not page-aligned");

    struct Vma *vma = vma_lookup(p, va);
    TEST_ASSERT(vma != NULL, "no VMA at the lazy-attached VA");
    TEST_EXPECT_EQ(vma->vaddr_start, va,             "lazy VMA start == attach VA");
    TEST_EXPECT_EQ(vma->vaddr_end,   va + PAGE_SIZE, "lazy VMA spans one page");
    TEST_EXPECT_EQ(vma->prot,        VMA_PROT_RW,    "lazy VMA prot is RW");

    // Tier 1: the VMA owns the Burrow -- mapping_count 1, handle_count 0.
    struct Burrow *b = vma->burrow;
    TEST_ASSERT(b != NULL, "lazy VMA has no backing Burrow");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "lazy Burrow mapping_count == 1");
    TEST_EXPECT_EQ(burrow_handle_count(b),  0, "lazy Burrow handle_count == 0");

    // A lazy reservation commits no pages (page_count 0) but charges the VMA axis.
    TEST_EXPECT_EQ((u64)p->as->page_count, 0ull, "lazy attach charges no pages (free reservation)");
    TEST_EXPECT_EQ((u64)p->as->vma_count,  1ull, "lazy attach charges one VMA (I-32 4th axis)");

    drop_proc(p);
}

// Audit F1 regression: a LAZY reservation must NOT be capped at the eager
// BURROW_ATTACH_MAX (256 MiB) -- a lazy region commits no pages at attach, so the
// reservation can be large (Go-stock reserves a ~512-MiB page-summary). The lazy
// cap is BURROW_RESERVE_MAX (1 GiB). Pre-fix, the lazy attach reused the 256-MiB
// eager cap and -1'd a 512-MiB reservation, making the #321 Go-stock proof impossible.
void test_sys_burrow_attach_lazy_large(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // 512 MiB -- ABOVE the eager 256-MiB cap, BELOW the 1-GiB reservation cap. The
    // eager SYS_BURROW_ATTACH would -1 this; the lazy attach must SUCCEED, committing
    // NO pages (the free reservation -> page_count stays 0).
    const u64 BIG = 512ull * 1024 * 1024;
    s64 r = sys_burrow_attach_lazy_for_proc(p, BIG);
    TEST_ASSERT(r > 0, "lazy attach of 512 MiB (> BURROW_ATTACH_MAX) must succeed");
    u64 va = (u64)r;
    TEST_ASSERT(in_window(va), "big lazy attach VA in the burrow window");
    struct Vma *vma = vma_lookup(p, va);
    TEST_ASSERT(vma != NULL && vma->vaddr_end - vma->vaddr_start == BIG,
        "the 512-MiB VMA spans the whole reservation");
    TEST_EXPECT_EQ((u64)p->as->page_count, 0ull,
        "a 512-MiB lazy reservation commits NO pages (page_count 0)");

    // Above BURROW_RESERVE_MAX (1 GiB) is still rejected (the lazy reservation cap).
    TEST_EXPECT_EQ(sys_burrow_attach_lazy_for_proc(p, BURROW_RESERVE_MAX + PAGE_SIZE),
                   -1L, "lazy attach above BURROW_RESERVE_MAX rejected");

    drop_proc(p);
}

// CL-4: the dual-ABI length selection. SYS_BURROW_ATTACH_LAZY accepts the
// native 1-arg form AND the Linux 6-arg anon-mmap form (musl's __init_tls
// issues the latter raw). The gate must honour the 6-arg reading ONLY for an
// anonymous-private request: the pouch wrapper's file-backed / MAP_FIXED
// refusals are libc-side, so a direct syscall(SYS_mmap, ...) would otherwise
// get anonymous zero pages where it asked for a FILE -- silently wrong data
// where pre-CL-4 it was a loud MAP_FAILED (ARCH 6.5).
void test_sys_burrow_lazy_len_from_args(void) {
    enum { MAP_PRIVATE = 0x02, MAP_FIXED = 0x10, MAP_ANON = 0x20 };
    const u64 LEN = 64ull * 1024;
    const u64 NOFD = (u64)(s64)-1;

    // Native 1-arg: length in x0, x1..x5 undefined -- must ignore them entirely.
    TEST_EXPECT_EQ(burrow_lazy_len_from_args(LEN, 0xdeadbeef, 0xdeadbeef, 7),
                   LEN, "native 1-arg takes x0 regardless of the other regs");

    // Linux 6-arg anonymous-private (what __init_tls issues): length from x1.
    TEST_EXPECT_EQ(burrow_lazy_len_from_args(0, LEN, MAP_ANON | MAP_PRIVATE, NOFD),
                   LEN, "6-arg anonymous-private takes the length from x1");

    // FILE-BACKED must fail closed -- this is the F1 regression.
    TEST_EXPECT_EQ(burrow_lazy_len_from_args(0, LEN, MAP_PRIVATE, 7),
                   0ull, "6-arg file-backed mmap refused (no anonymous fallback)");

    // MAP_FIXED picks the VA; the kernel chooses, so refuse.
    TEST_EXPECT_EQ(burrow_lazy_len_from_args(0, LEN, MAP_ANON | MAP_FIXED, NOFD),
                   0ull, "6-arg MAP_FIXED refused");

    // Anonymous but carrying a real fd is malformed -- refuse rather than guess.
    TEST_EXPECT_EQ(burrow_lazy_len_from_args(0, LEN, MAP_ANON | MAP_PRIVATE, 3),
                   0ull, "6-arg anonymous with a real fd refused");

    // The native wrappers pin x1 = 0, so a native length of 0 stays a clean
    // reject instead of depending on whatever the compiler left in x1.
    TEST_EXPECT_EQ(burrow_lazy_len_from_args(0, 0, 0, NOFD),
                   0ull, "length 0 with x1 pinned 0 stays a reject");
}

// =============================================================================
// DISTRO D-3b -- the MAP_FIXED split/replace surgery.
//
// This is the audit-bearing half of D-3b: it is the FIRST producer of a
// non-zero vma->burrow_offset in the tree (grep says every other vma_alloc call
// passes a literal 0, or copies one that can only be 0), so the arithmetic these
// cases exercise has never run in production before.
//
// Every case asserts BYTE IDENTITY on the survivors, not merely their bounds. A
// split that gets the bounds right and the offset wrong yields a VMA that looks
// perfect and serves the wrong page -- which is precisely the corruption the
// #190 fix exists to refuse, arriving from the other direction.
// =============================================================================

#define D3B_PAGES 8
#define D3B_LEN   ((u64)D3B_PAGES * PAGE_SIZE)

// Attach an 8-page lazy anon mapping and return its base, or 0 on failure --
// TEST_ASSERT returns void, so the assertion belongs at each call site. The VMA
// this produces has burrow_offset 0, which is what every real mapping in the
// tree looked like before D-3b.
static u64 d3b_base(struct Proc *p) {
    s64 r = sys_burrow_attach_lazy_for_proc(p, D3B_LEN);
    return r > 0 ? (u64)r : 0;
}

// Replace the TAIL of a VMA: survivor on the left, no right remainder. This is
// the shape EVERY real split takes on the stock Alpine rootfs -- map_library's
// span ends exactly at the last PT_LOAD's page-rounded end, so the overlay
// always reaches the end of the reservation.
void test_burrow_map_fixed_split_left(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    // Replace the last 3 pages.
    u64 cut = base + 5 * PAGE_SIZE;
    struct Burrow *nb = fresh_anon(3 * PAGE_SIZE);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, cut, 3 * PAGE_SIZE, VMA_PROT_RW, 0),
                   0, "tail replace must succeed");
    burrow_unref(nb);

    struct Vma *left = vma_lookup(p, base);
    struct Vma *mid  = vma_lookup(p, cut);
    TEST_ASSERT(left && mid, "both pieces must exist");
    TEST_ASSERT(left != mid, "the pieces must be distinct VMAs");
    TEST_EXPECT_EQ(left->vaddr_start, base, "left keeps the original start");
    TEST_EXPECT_EQ(left->vaddr_end,   cut,  "left ends at the cut");
    TEST_EXPECT_EQ(mid->vaddr_start,  cut,  "mid starts at the cut");
    TEST_EXPECT_EQ(mid->vaddr_end, base + D3B_LEN, "mid runs to the old end");
    TEST_ASSERT(mid->burrow == nb, "mid carries the NEW backing");

    // The left survivor's bytes are untouched: same Burrow, same identity.
    TEST_EXPECT_EQ(left->burrow_offset, 0ull, "left keeps offset 0");
    TEST_EXPECT_EQ(byte_identity(left, base + 4 * PAGE_SIZE),
                   4ull * PAGE_SIZE, "left byte identity unchanged");

    drop_proc(p);
}

// Replace the HEAD: survivor on the right. THE CASE THAT MOVES burrow_offset --
// the right remainder must advance its offset by exactly the same delta as its
// start, or every byte it serves is shifted.
void test_burrow_map_fixed_split_right(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    u64 cut = base + 3 * PAGE_SIZE;             // replace the first 3 pages
    struct Burrow *nb = fresh_anon(3 * PAGE_SIZE);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, base, 3 * PAGE_SIZE, VMA_PROT_RW, 0),
                   0, "head replace must succeed");
    burrow_unref(nb);

    struct Vma *mid   = vma_lookup(p, base);
    struct Vma *right = vma_lookup(p, cut);
    TEST_ASSERT(mid && right, "both pieces must exist");
    TEST_ASSERT(mid != right, "the pieces must be distinct VMAs");
    TEST_EXPECT_EQ(mid->vaddr_start,   base, "mid starts at the base");
    TEST_EXPECT_EQ(mid->vaddr_end,     cut,  "mid ends at the cut");
    TEST_EXPECT_EQ(right->vaddr_start, cut,  "right starts at the cut");
    TEST_EXPECT_EQ(right->vaddr_end, base + D3B_LEN, "right runs to the old end");

    // THE LOAD-BEARING ASSERTION. Before the split, base+5*PAGE named byte
    // 5*PAGE of the Burrow. It must still.
    TEST_EXPECT_EQ(right->burrow_offset, 3ull * PAGE_SIZE,
                   "right advances its offset by the cut delta");
    TEST_EXPECT_EQ(byte_identity(right, base + 5 * PAGE_SIZE),
                   5ull * PAGE_SIZE, "right byte identity unchanged by the split");
    TEST_EXPECT_EQ(byte_identity(right, cut), 3ull * PAGE_SIZE,
                   "the first surviving page still names its own byte");

    drop_proc(p);
}

// Replace the MIDDLE: survivors on BOTH sides. Unreachable from the stock
// rootfs (see the left-split note), so this is its only coverage.
void test_burrow_map_fixed_split_three_way(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    u64 lo = base + 2 * PAGE_SIZE, hi = base + 5 * PAGE_SIZE;
    struct Burrow *nb = fresh_anon(hi - lo);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, lo, (size_t)(hi - lo), VMA_PROT_RW, 0),
                   0, "interior replace must succeed");
    burrow_unref(nb);

    struct Vma *left  = vma_lookup(p, base);
    struct Vma *mid   = vma_lookup(p, lo);
    struct Vma *right = vma_lookup(p, hi);
    TEST_ASSERT(left && mid && right, "all three pieces must exist");
    TEST_ASSERT(left != mid && mid != right && left != right,
                "the three pieces must be distinct VMAs");
    TEST_EXPECT_EQ(left->vaddr_end,     lo, "left ends at the low cut");
    TEST_EXPECT_EQ(mid->vaddr_start,    lo, "mid starts at the low cut");
    TEST_EXPECT_EQ(mid->vaddr_end,      hi, "mid ends at the high cut");
    TEST_EXPECT_EQ(right->vaddr_start,  hi, "right starts at the high cut");
    TEST_EXPECT_EQ(right->vaddr_end, base + D3B_LEN, "right runs to the old end");

    // Both survivors keep byte identity, and they are backed by the SAME
    // Burrow -- the split did not duplicate it.
    TEST_ASSERT(left->burrow == right->burrow, "survivors share the old Burrow");
    TEST_EXPECT_EQ(byte_identity(left,  base + PAGE_SIZE),     1ull * PAGE_SIZE,
                   "left byte identity unchanged");
    TEST_EXPECT_EQ(byte_identity(right, base + 6 * PAGE_SIZE), 6ull * PAGE_SIZE,
                   "right byte identity unchanged");

    drop_proc(p);
}

// Replace the WHOLE VMA: no remainder. The one case that removes the old VMA,
// and therefore the one whose rollback has to put a mapping back.
void test_burrow_map_fixed_exact_cover(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    struct Burrow *nb = fresh_anon(D3B_LEN);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, base, (size_t)D3B_LEN, VMA_PROT_RW, 0),
                   0, "exact-cover replace must succeed");

    struct Vma *only = vma_lookup(p, base);
    TEST_ASSERT(only != NULL, "the replacement must exist");
    TEST_EXPECT_EQ(only->vaddr_start, base,            "start preserved");
    TEST_EXPECT_EQ(only->vaddr_end,   base + D3B_LEN,  "end preserved");
    TEST_ASSERT(only->burrow == nb, "the new backing replaced the old");
    // The mapping now owns the Burrow (mapping_count 1); our construction ref
    // is still outstanding, so drop it and confirm the mapping keeps it alive.
    TEST_EXPECT_EQ(burrow_mapping_count(nb), 1, "exactly one mapping");
    burrow_unref(nb);
    TEST_ASSERT(vma_lookup(p, base) != NULL, "the mapping survives the unref");

    drop_proc(p);
}

// A non-zero burrow_offset on the REPLACEMENT is carried through verbatim --
// this is what lets a caller name a window into the middle of a backing object.
void test_burrow_map_fixed_carries_offset(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    struct Burrow *nb = fresh_anon(4 * PAGE_SIZE);
    TEST_ASSERT(nb != NULL, "anon create failed");
    u64 cut = base + 2 * PAGE_SIZE;
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, cut, 2 * PAGE_SIZE, VMA_PROT_RW,
                                    2 * PAGE_SIZE),
                   0, "offset replace must succeed");
    burrow_unref(nb);

    struct Vma *mid = vma_lookup(p, cut);
    TEST_ASSERT(mid != NULL, "the replacement must exist");
    TEST_EXPECT_EQ(mid->burrow_offset, 2ull * PAGE_SIZE, "offset carried verbatim");
    TEST_EXPECT_EQ(byte_identity(mid, cut), 2ull * PAGE_SIZE,
                   "the window names the requested byte, not byte 0");

    drop_proc(p);
}

// Every refusal, and each one asserted to have changed NOTHING. A surgery that
// refuses but has already cut is worse than one that never ran.
void test_burrow_map_fixed_refusals(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    struct Burrow *nb = fresh_anon(D3B_LEN);
    TEST_ASSERT(nb != NULL, "anon create failed");

    // NOTE an unmapped target is NOT here, and that is the #196 correction: a
    // fixed map into free space SUCCEEDS (Linux places it there rather than
    // failing). burrow.map_fixed_into_free_space covers it positively, together
    // with the partial-overlap range that IS still refused.

    // Runs off the END of the covering VMA -- the window must be WHOLLY inside.
    TEST_ASSERT(map_fixed_locked(p, nb, base + 6 * PAGE_SIZE, 4 * PAGE_SIZE,
                                 VMA_PROT_RW, 0) != 0,
                "a window overrunning the VMA is refused");

    // Misaligned address and length.
    TEST_ASSERT(map_fixed_locked(p, nb, base + 1, PAGE_SIZE, VMA_PROT_RW, 0) != 0,
                "a misaligned address is refused");
    TEST_ASSERT(map_fixed_locked(p, nb, base, PAGE_SIZE + 1, VMA_PROT_RW, 0) != 0,
                "a misaligned length is refused");
    TEST_ASSERT(map_fixed_locked(p, nb, base, 0, VMA_PROT_RW, 0) != 0,
                "a zero length is refused");

    // W+X: vma_alloc rejects it, and the surgery must surface that as a clean
    // refusal rather than a half-applied split (I-12).
    TEST_ASSERT(map_fixed_locked(p, nb, base + PAGE_SIZE, PAGE_SIZE,
                                 VMA_PROT_WRITE | VMA_PROT_EXEC | VMA_PROT_READ,
                                 0) != 0,
                "a W+X replacement is refused");

    // A FLAGGED survivor is refused: SHARED_IN is another Proc's memory carrying
    // a per-span budget charge, and COW would need per-page share counts
    // reasoned across the cut. Set the flag directly -- building a real shared
    // mapping here would test burrow_share_into, not this.
    struct Vma *v = vma_lookup(p, base);
    TEST_ASSERT(v != NULL, "the base VMA must exist");
    u32 saved = v->flags;
    v->flags = VMA_FLAG_COW;
    TEST_ASSERT(map_fixed_locked(p, nb, base + PAGE_SIZE, PAGE_SIZE,
                                 VMA_PROT_RW, 0) != 0,
                "a flagged (COW) survivor is refused");
    v->flags = VMA_FLAG_SHARED_IN;
    TEST_ASSERT(map_fixed_locked(p, nb, base + PAGE_SIZE, PAGE_SIZE,
                                 VMA_PROT_RW, 0) != 0,
                "a flagged (SHARED_IN) survivor is refused");
    v->flags = saved;

    // NOTHING above may have cut. One VMA, original bounds, original identity.
    struct Vma *still = vma_lookup(p, base);
    TEST_ASSERT(still != NULL, "the original mapping must survive every refusal");
    TEST_EXPECT_EQ(still->vaddr_start, base,           "start untouched");
    TEST_EXPECT_EQ(still->vaddr_end,   base + D3B_LEN, "end untouched");
    TEST_EXPECT_EQ(still->burrow_offset, 0ull,         "offset untouched");
    TEST_ASSERT(vma_lookup(p, base + 7 * PAGE_SIZE) == still,
                "the whole span is still ONE VMA -- no refusal left a seam");

    burrow_unref(nb);
    drop_proc(p);
}

// Successive overlays compose, which is what map_library actually does: the
// span is cut once per PT_LOAD past the lowest, each cut landing inside the
// remainder the previous one left.
void test_burrow_map_fixed_successive(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    struct Burrow *a = fresh_anon(2 * PAGE_SIZE);
    struct Burrow *b = fresh_anon(2 * PAGE_SIZE);
    TEST_ASSERT(a && b, "anon create failed");

    // First overlay: pages 2-3. Second: pages 6-7, inside the right remainder.
    TEST_EXPECT_EQ(map_fixed_locked(p, a, base + 2 * PAGE_SIZE, 2 * PAGE_SIZE,
                                    VMA_PROT_RW, 0), 0, "first overlay");
    TEST_EXPECT_EQ(map_fixed_locked(p, b, base + 6 * PAGE_SIZE, 2 * PAGE_SIZE,
                                    VMA_PROT_RW, 0), 0, "second overlay");
    burrow_unref(a);
    burrow_unref(b);

    // Four pieces: [0,2) old, [2,4) a, [4,6) old, [6,8) b.
    struct Vma *p0 = vma_lookup(p, base);
    struct Vma *p1 = vma_lookup(p, base + 2 * PAGE_SIZE);
    struct Vma *p2 = vma_lookup(p, base + 4 * PAGE_SIZE);
    struct Vma *p3 = vma_lookup(p, base + 6 * PAGE_SIZE);
    TEST_ASSERT(p0 && p1 && p2 && p3, "all four pieces must exist");
    TEST_ASSERT(p0 != p1 && p1 != p2 && p2 != p3, "pieces must be distinct");
    TEST_EXPECT_EQ(p0->vaddr_end,   base + 2 * PAGE_SIZE, "piece 0 bounds");
    TEST_EXPECT_EQ(p1->vaddr_end,   base + 4 * PAGE_SIZE, "piece 1 bounds");
    TEST_EXPECT_EQ(p2->vaddr_start, base + 4 * PAGE_SIZE, "piece 2 bounds");
    TEST_EXPECT_EQ(p2->vaddr_end,   base + 6 * PAGE_SIZE, "piece 2 end");
    TEST_EXPECT_EQ(p3->vaddr_start, base + 6 * PAGE_SIZE, "piece 3 bounds");

    // The two OLD survivors both still name their own bytes, though only one of
    // them ever had its offset moved.
    TEST_ASSERT(p0->burrow == p2->burrow, "both survivors share the old Burrow");
    TEST_EXPECT_EQ(byte_identity(p0, base + PAGE_SIZE),     1ull * PAGE_SIZE,
                   "survivor 0 byte identity");
    TEST_EXPECT_EQ(byte_identity(p2, base + 5 * PAGE_SIZE), 5ull * PAGE_SIZE,
                   "survivor 2 byte identity after TWO overlays");

    drop_proc(p);
}

// A MAP_FIXED into FREE space -- no VMA to split. THE CASE THE SPLIT TESTS
// ABOVE ARE STRUCTURALLY BLIND TO: each of them attaches a mapping first, so
// none could ever reach the no-covering-VMA arm. That blindness is exactly how
// #196 shipped past a green suite and was caught only by the in-guest probe.
//
// Linux MAP_FIXED does not require the range to be mapped already; refusing
// here made the shell answer ENOMEM, which an allocator reads as OOM.
void test_burrow_map_fixed_into_free_space(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // Anchor inside the burrow window, then map at a DISTANT free address so
    // nothing covers it. The anchor exists only to prove the address space is
    // otherwise live -- the target itself must be unmapped.
    u64 anchor = d3b_base(p);
    TEST_ASSERT(anchor != 0, "lazy attach failed");
    u64 target = anchor + 256 * PAGE_SIZE;
    TEST_ASSERT(vma_lookup(p, target) == NULL, "the target must be unmapped");

    struct Burrow *nb = fresh_anon(2 * PAGE_SIZE);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, target, 2 * PAGE_SIZE, VMA_PROT_RW, 0),
                   0, "a fixed map into free space must SUCCEED, not ENOMEM");
    burrow_unref(nb);

    struct Vma *v = vma_lookup(p, target);
    TEST_ASSERT(v != NULL, "the mapping must exist at the requested address");
    TEST_EXPECT_EQ(v->vaddr_start, target, "it lands EXACTLY where asked");
    TEST_EXPECT_EQ(v->vaddr_end, target + 2 * PAGE_SIZE, "with the asked span");
    TEST_ASSERT(v->burrow == nb, "backed by the requested Burrow");

    // The anchor is untouched -- a free-space map splits nothing.
    struct Vma *a = vma_lookup(p, anchor);
    TEST_ASSERT(a != NULL, "the anchor survives");
    TEST_EXPECT_EQ(a->vaddr_end, anchor + D3B_LEN, "the anchor was not cut");

    // PARTIAL OVERLAP still refuses: a range that starts free and runs INTO an
    // existing VMA is neither shape. Straddle the anchor's start.
    struct Burrow *ob = fresh_anon(4 * PAGE_SIZE);
    TEST_ASSERT(ob != NULL, "anon create failed");
    TEST_ASSERT(map_fixed_locked(p, ob, anchor - 2 * PAGE_SIZE, 4 * PAGE_SIZE,
                                 VMA_PROT_RW, 0) != 0,
                "a range straddling a VMA boundary is refused");
    TEST_ASSERT(vma_lookup(p, anchor) == a, "the straddled VMA is unchanged");
    TEST_EXPECT_EQ(a->vaddr_start, anchor, "its start is unchanged");
    burrow_unref(ob);

    drop_proc(p);
}

// =============================================================================
// #199 (D-3c): sys_munmap_range_for_proc -- the whole-VMA range detach the
// phenotype munmap row serves. D-3b's split turns one library map into 2-3
// VMAs; musl's unmap_library munmaps the WHOLE span in one call, which the
// exact-match detach refused, leaking the library. The range form detaches
// every VMA wholly inside (each one WHOLE -- never partial), succeeds on an
// empty range (Linux no-op), and refuses ATOMICALLY on a boundary straddle.
// =============================================================================

extern s64 sys_munmap_range_for_proc(struct Proc *p, u64 vaddr_raw,
                                     u64 length_raw);

// The musl unmap_library shape: a lazy reservation cut into three VMAs by a
// MAP_FIXED overlay, then ONE whole-span munmap. All three go; the space is
// reusable; the I-32 counters return to their pre-attach values.
void test_burrow_munmap_range_tiled(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u32 pages0 = p->as->page_count;
    u32 vmas0  = p->as->vma_count;

    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    // Cut [base+2P, base+5P) out of the middle: left | mid | right = 3 VMAs.
    u64 cut = base + 2 * PAGE_SIZE;
    struct Burrow *nb = fresh_anon(3 * PAGE_SIZE);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, cut, 3 * PAGE_SIZE, VMA_PROT_RW, 0),
                   0, "three-way split must succeed");
    burrow_unref(nb);
    TEST_ASSERT(vma_lookup(p, base) && vma_lookup(p, cut) &&
                vma_lookup(p, base + 5 * PAGE_SIZE), "three pieces exist");

    // The musl call: munmap(dso->map, dso->map_len) -- the WHOLE span.
    TEST_EXPECT_EQ(sys_munmap_range_for_proc(p, base, D3B_LEN), 0,
                   "whole-span munmap over 3 VMAs succeeds");
    TEST_ASSERT(vma_lookup(p, base) == NULL, "left piece gone");
    TEST_ASSERT(vma_lookup(p, cut) == NULL, "mid piece gone");
    TEST_ASSERT(vma_lookup(p, base + 5 * PAGE_SIZE) == NULL, "right piece gone");

    // The counters return to baseline: the lazy pieces refund their resident
    // count (0 -- nothing faulted) and the eager mid refunds only a RECORDED
    // charge (none -- the test mapped it directly, nothing billed page_count),
    // the #122 positive-allowlist working as attribution.
    TEST_EXPECT_EQ((u64)p->as->page_count, (u64)pages0, "page axis back to baseline");
    TEST_EXPECT_EQ((u64)p->as->vma_count,  (u64)vmas0,  "VMA axis back to baseline");

    // The space is genuinely free: a fresh attach can land again.
    u64 again = d3b_base(p);
    TEST_ASSERT(again != 0, "the range is reusable after the range munmap");

    drop_proc(p);
}

// A range that cuts INTO a VMA (its start lies outside) is TRUE partial unmap:
// refused whole, nothing detached -- the atomicity that keeps "munmap returned
// an error" meaning "your mappings are exactly as they were".
void test_burrow_munmap_range_partial_refused(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");

    u64 cut = base + 4 * PAGE_SIZE;
    struct Burrow *nb = fresh_anon(4 * PAGE_SIZE);
    TEST_ASSERT(nb != NULL, "anon create failed");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, cut, 4 * PAGE_SIZE, VMA_PROT_RW, 0),
                   0, "tail replace must succeed");
    burrow_unref(nb);

    // [base+P, end): the first VMA straddles the range's left edge. The SECOND
    // VMA lies wholly inside -- and must SURVIVE the refusal untouched.
    TEST_EXPECT_EQ(sys_munmap_range_for_proc(p, base + PAGE_SIZE,
                                             D3B_LEN - PAGE_SIZE),
                   -1, "boundary-straddling range refused");
    struct Vma *left = vma_lookup(p, base);
    struct Vma *mid  = vma_lookup(p, cut);
    TEST_ASSERT(left != NULL, "straddled VMA survives");
    TEST_ASSERT(mid  != NULL, "the wholly-inside VMA ALSO survives (atomic refusal)");
    TEST_EXPECT_EQ(left->vaddr_end, cut, "straddled VMA geometry untouched");

    drop_proc(p);
}

// Linux munmap over nothing is a success no-op. The exact-match detach called
// this -1; the phenotype row needs the honest 0.
void test_burrow_munmap_range_empty_ok(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // A window inside the attach range with nothing mapped: find one by
    // attaching, remembering, and detaching -- the hole left behind is known
    // to be inside the window bounds.
    u64 base = d3b_base(p);
    TEST_ASSERT(base != 0, "lazy attach failed");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, base, D3B_LEN), 0,
                   "exact detach clears the range");

    TEST_EXPECT_EQ(sys_munmap_range_for_proc(p, base, D3B_LEN), 0,
                   "munmap of an empty range is the Linux no-op success");

    drop_proc(p);
}

// =============================================================================
// #F1 (D-3c FOCUSED round): the teardown twin of #193. A FILE Burrow's free
// reaches spoor_clunk -> dev->close, which may sleep (a 9P Tclunk); the detach
// / munmap / drain paths run under as->lock, so an INLINE free is the
// lock-across-sleep extinction. The fix defers the free past the unlock.
//
// This does not need a REAL sleep to discriminate: a stub Dev's .close records
// current_thread()->preempt_count at free time. Pre-fix the free runs under
// as->lock (spin_lock bumped preempt_count -> the hook sees >= 1); post-fix it
// runs after the unlock (0). The assertion is on the RECORDED value, so it is
// deterministic rather than relying on an actual sleep to trip the extinction.
// =============================================================================

static int g_f1_close_calls   = 0;
static u32 g_f1_close_preempt = 0xffffffffu;   // sentinel: "close never ran"

static void f1_stub_close(struct Spoor *c) {
    (void)c;
    g_f1_close_calls++;
    struct Thread *t = current_thread();
    g_f1_close_preempt = t ? t->preempt_count : 0xdeadu;
}

// A minimal read so the FILE Burrow's backing Dev is byte-I/O-able (never
// actually read here -- the mapping is torn down before any fault).
static long f1_stub_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)off;
    for (long i = 0; i < n; i++) ((u8 *)buf)[i] = 0;
    return n;
}

static struct Dev g_f1_dev = {
    .dc    = 'F',
    .name  = "f1test",
    .read  = f1_stub_read,
    .close = f1_stub_close,
};

// Build a mapped, in-window FILE Burrow whose ONLY ref is the mapping (handle
// dropped) -- so detach's mapping-drop is the last ref and MUST free. Returns
// the VA, or 0 on setup failure.
static u64 f1_map_file(struct Proc *p, u64 va) {
    struct Spoor *s = spoor_alloc(&g_f1_dev);
    if (!s) return 0;
    s->qid.type = QTFILE;
    struct Burrow *v = burrow_create_file(s, 0, PAGE_SIZE);   // adopts s
    if (!v) { spoor_clunk(s); return 0; }
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, v, va, PAGE_SIZE, VMA_PROT_READ);
    spin_unlock(&p->as->lock);
    if (rc != 0) { burrow_unref(v); return 0; }
    burrow_unref(v);          // drop the construction handle: mapping is the last ref
    return va;
}

void test_burrow_detach_file_frees_outside_lock(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_f1_close_calls = 0; g_f1_close_preempt = 0xffffffffu;
    u64 va = f1_map_file(p, EXEC_USER_BURROW_BASE);
    TEST_ASSERT(va != 0, "FILE burrow map setup failed");

    // The exact detach: the mapping-drop is the last ref -> the Burrow frees ->
    // spoor_clunk (last spoor ref) -> f1_stub_close.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, PAGE_SIZE), (s64)0,
                   "detach of a mapped FILE burrow succeeds");
    TEST_EXPECT_EQ(g_f1_close_calls, 1, "the backing Dev's close ran (Burrow freed)");
    TEST_EXPECT_EQ((u64)g_f1_close_preempt, 0ull,
                   "the free ran with as->lock DROPPED (preempt_count 0) -- #F1");

    drop_proc(p);
}

void test_burrow_munmap_range_file_frees_outside_lock(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_f1_close_calls = 0; g_f1_close_preempt = 0xffffffffu;
    // Two adjacent FILE burrows, torn down by ONE range munmap -- the multi-free
    // path (the deferred_free_next chain), which is exactly the amplified shape
    // (musl's whole-span unmap_library over several segment Images).
    u64 a = f1_map_file(p, EXEC_USER_BURROW_BASE);
    u64 b = f1_map_file(p, EXEC_USER_BURROW_BASE + PAGE_SIZE);
    TEST_ASSERT(a != 0 && b != 0, "two-FILE-burrow setup failed");

    TEST_EXPECT_EQ(sys_munmap_range_for_proc(p, a, 2 * PAGE_SIZE), (s64)0,
                   "range munmap over two FILE burrows succeeds");
    TEST_EXPECT_EQ(g_f1_close_calls, 2, "both backing Devs closed (both freed)");
    TEST_EXPECT_EQ((u64)g_f1_close_preempt, 0ull,
                   "the deferred frees ran with as->lock DROPPED -- #F1");

    drop_proc(p);
}
