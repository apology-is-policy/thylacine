// #65 (invariant I-32): the per-Proc resource floor.
//
// The floor caps a non-TCB Proc's anonymous pages (SYS_BURROW_ATTACH), threads
// (SYS_THREAD_SPAWN), and direct children (rfork) so a fork/thread/memory bomb
// is bounded, not box-extincting. The TCB (PRINCIPAL_SYSTEM) is exempt, and the
// exemption is unforgeable. These tests cover the cap LOGIC (the predicate
// helpers), the counter maintenance, and a light syscall-path integration that
// hits the page cap WITHOUT allocating 256 MiB (it pre-sets page_count near the
// cap and attaches a 1-/2-page request across the boundary).
//
//   resource.exempt_only_system
//     proc_resource_exempt is true ONLY for PRINCIPAL_SYSTEM -- a real user,
//     PRINCIPAL_NONE, PRINCIPAL_INVALID, and NULL are all non-exempt.
//   resource.page_charge_caps
//     proc_page_charge refuses a non-exempt Proc over PROC_PAGE_MAX (charging
//     nothing), uncharge clamps at 0, and the overflow guard refuses a wrapping
//     charge. proc_page_charge on an exempt Proc bypasses the cap.
//   resource.thread_cap_ok / resource.child_cap_ok
//     the spawn-gate predicates: non-exempt below the cap OK, at/over the cap
//     refused; exempt always OK; NULL refused (fail-closed).
//   resource.child_count_tracks_list
//     proc_link_child / proc_unlink_child keep child_count == the children-list
//     length (driven via proc_test_link / proc_test_unlink on kproc).
//   resource.page_cap_attach_enforced
//     the REAL sys_burrow_attach_for_proc path: a 2-page attach one-below-cap
//     returns -ENOMEM and allocates nothing; a 1-page attach at the boundary
//     succeeds and charges; detach uncharges; an exempt Proc bypasses the cap.

#include "test.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

#include "../include/thylacine/errno.h"

// The _for_proc inners of the burrow SVC handlers (non-static cores in
// kernel/syscall.c), driven directly -- the same pattern test_sys_burrow uses.
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

// Test-only Proc list helpers (no production caller; defined in kernel/proc.c).
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);

void test_resource_exempt_only_system(void);
void test_resource_page_charge_caps(void);
void test_resource_thread_cap_ok(void);
void test_resource_child_cap_ok(void);
void test_resource_child_count_tracks_list(void);
void test_resource_child_count_rfork_reap(void);
void test_resource_page_cap_attach_enforced(void);
void test_resource_vma_cap(void);

#define A_REAL_USER 1000u

static struct Proc *res_make(u32 principal) {
    struct Proc *p = proc_alloc();
    if (!p) extinction("test_resource: proc_alloc failed");
    p->principal_id = principal;       // immutable-on-running; safe to set on a
                                       // detached test Proc before any use.
    return p;
}

static void res_drop(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

static void res_child_thunk(void *arg) { (void)arg; exits("ok"); }

void test_resource_exempt_only_system(void) {
    struct Proc *sys  = res_make((u32)PRINCIPAL_SYSTEM);
    struct Proc *user = res_make(A_REAL_USER);
    struct Proc *none = res_make((u32)PRINCIPAL_NONE);
    struct Proc *inv  = res_make((u32)PRINCIPAL_INVALID);

    TEST_ASSERT(proc_resource_exempt(sys),   "PRINCIPAL_SYSTEM must be exempt");
    TEST_ASSERT(!proc_resource_exempt(user), "a real user must NOT be exempt");
    TEST_ASSERT(!proc_resource_exempt(none), "PRINCIPAL_NONE must NOT be exempt");
    TEST_ASSERT(!proc_resource_exempt(inv),  "PRINCIPAL_INVALID must NOT be exempt");
    TEST_ASSERT(!proc_resource_exempt(NULL), "NULL must be non-exempt (fail-closed)");

    res_drop(sys); res_drop(user); res_drop(none); res_drop(inv);
}

void test_resource_page_charge_caps(void) {
    struct Proc *p = res_make(A_REAL_USER);   // non-exempt

    // Charge exactly to the cap, then one more must be refused (charging
    // nothing). proc_page_charge only moves the COUNTER -- no real allocation.
    TEST_ASSERT(proc_page_charge(p, PROC_PAGE_MAX), "charge to the cap must succeed");
    TEST_EXPECT_EQ(p->page_count, PROC_PAGE_MAX, "page_count == cap after full charge");
    TEST_ASSERT(!proc_page_charge(p, 1u), "charge past the cap must be refused");
    TEST_EXPECT_EQ(p->page_count, PROC_PAGE_MAX, "a refused charge charges nothing");

    // Uncharge, then re-charge the freed pages.
    proc_page_uncharge(p, 100u);
    TEST_EXPECT_EQ(p->page_count, PROC_PAGE_MAX - 100u, "uncharge 100");
    TEST_ASSERT(proc_page_charge(p, 100u), "re-charge the freed 100 pages");
    TEST_EXPECT_EQ(p->page_count, PROC_PAGE_MAX, "back at the cap");

    // Uncharge everything, then an over-uncharge clamps at 0 (no underflow).
    proc_page_uncharge(p, PROC_PAGE_MAX);
    TEST_EXPECT_EQ(p->page_count, 0u, "full uncharge -> 0");
    proc_page_uncharge(p, 50u);
    TEST_EXPECT_EQ(p->page_count, 0u, "over-uncharge clamps at 0 (no underflow)");

    // Overflow guard: a charge whose sum would wrap u32 is refused even though
    // it is "under the cap" arithmetic-wise after wrap.
    TEST_ASSERT(proc_page_charge(p, 1u), "charge 1");
    TEST_ASSERT(!proc_page_charge(p, 0xFFFFFFFFu), "overflowing charge refused");
    TEST_EXPECT_EQ(p->page_count, 1u, "the overflowing charge charged nothing");

    // Exempt Procs bypass the cap entirely.
    struct Proc *sys = res_make((u32)PRINCIPAL_SYSTEM);
    TEST_ASSERT(proc_page_charge(sys, PROC_PAGE_MAX), "exempt charge to cap");
    TEST_ASSERT(proc_page_charge(sys, PROC_PAGE_MAX), "exempt charge PAST the cap");
    TEST_EXPECT_EQ(sys->page_count, 2u * PROC_PAGE_MAX, "exempt is unbounded by the cap");

    res_drop(p); res_drop(sys);
}

// CL-5: page_peak is the high-water mark of page_count -- the number the F4
// budget is sized against, and the one thing a poller structurally cannot
// measure on a short-lived process. It must rise with the peak, NEVER fall on
// an uncharge (that is what makes a post-mortem read of a ZOMBIE exact), and a
// REFUSED charge must not move it (it charged nothing).
void test_resource_page_peak_high_water(void) {
    struct Proc *p = res_make(A_REAL_USER);
    TEST_EXPECT_EQ(p->page_peak, 0u, "a fresh Proc has no peak");

    TEST_ASSERT(proc_page_charge(p, 100u), "charge 100");
    TEST_EXPECT_EQ(p->page_peak, 100u, "peak tracks the first charge");

    TEST_ASSERT(proc_page_charge(p, 50u), "charge 50 more");
    TEST_EXPECT_EQ(p->page_peak, 150u, "peak follows page_count up");

    // The load-bearing property: releasing memory does NOT lower the peak.
    proc_page_uncharge(p, 140u);
    TEST_EXPECT_EQ(p->page_count, 10u, "uncharge lowered the live count");
    TEST_EXPECT_EQ(p->page_peak, 150u, "peak does NOT follow page_count down");

    // A re-charge below the previous peak leaves the peak alone.
    TEST_ASSERT(proc_page_charge(p, 20u), "charge back up to 30");
    TEST_EXPECT_EQ(p->page_count, 30u, "live count 30");
    TEST_EXPECT_EQ(p->page_peak, 150u, "peak unchanged below the high-water");

    // ...and one that exceeds it raises it.
    TEST_ASSERT(proc_page_charge(p, 200u), "charge past the old peak");
    TEST_EXPECT_EQ(p->page_peak, 230u, "peak rises past the old high-water");

    // A REFUSED charge charges nothing, so it must not move the peak either --
    // otherwise a Proc that merely ASKED for the cap would report having used it.
    u32 before = p->page_peak;
    TEST_ASSERT(!proc_page_charge(p, PROC_PAGE_MAX), "over-cap charge refused");
    TEST_EXPECT_EQ(p->page_peak, before, "a refused charge does not move the peak");
    TEST_ASSERT(!proc_page_charge(p, 0xFFFFFFFFu), "overflowing charge refused");
    TEST_EXPECT_EQ(p->page_peak, before, "a refused overflow does not move the peak");

    res_drop(p);
}

// CL-5: the spawn-time budget resolver -- the single authority decision.
// Rules: 0 inherits; <= the parent's own budget needs no authority (monotonic
// reduction, the I-2 shape); above it needs PROC_FLAG_MAY_RAISE_PAGE_BUDGET;
// over PROC_PAGE_HARD_MAX is refused for EVERYONE (the cap is what preserves
// the box-cliff protection, so no authority may exceed it).
void test_resource_spawn_budget_resolve(void) {
    struct Proc *plain = res_make(A_REAL_USER);      // no raise authority
    TEST_EXPECT_EQ(plain->page_budget, PROC_PAGE_MAX,
                   "a fresh Proc carries the default budget");

    // 0 == inherit. This is the compatibility contract: every pre-CL-5 caller
    // zero-fills sys_spawn_args, so it MUST resolve to the parent's budget.
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(plain, 0u), PROC_PAGE_MAX,
                   "0 inherits the parent's budget");

    // Reduction is always allowed -- a free sandboxing primitive.
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(plain, 1024u), 1024u,
                   "a smaller budget needs no authority");
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(plain, PROC_PAGE_MAX), PROC_PAGE_MAX,
                   "exactly the parent's budget needs no authority");

    // A RAISE without authority is refused (0 == refuse the spawn).
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(plain, PROC_PAGE_MAX + 1u), 0u,
                   "raising without authority is refused");

    // With the flag, the raise is granted...
    struct Proc *raiser = res_make(A_REAL_USER);
    proc_mark_may_raise_page_budget(raiser);
    TEST_ASSERT(proc_may_raise_page_budget(raiser), "the raise flag reads back");
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(raiser, PROC_PAGE_HARD_MAX),
                   PROC_PAGE_HARD_MAX, "an authorized raise to the hard cap");

    // ...but NEVER past the hard cap, flag or not. This is the property the
    // box-cliff protection rests on.
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(raiser, PROC_PAGE_HARD_MAX + 1u), 0u,
                   "even an authorized raise cannot exceed PROC_PAGE_HARD_MAX");
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(plain, 0xFFFFFFFFu), 0u,
                   "a wild budget is refused, not clamped");

    // Fail-closed on a NULL parent.
    TEST_EXPECT_EQ(proc_spawn_budget_resolve(NULL, 0u), 0u,
                   "NULL parent is refused (fail-closed)");

    res_drop(plain); res_drop(raiser);
}

// CL-5: the measurement that motivates the whole mechanism, pinned as a test.
// The clade gate measured a 1959-byte template-heavy C++ TU at 64066 pages
// (250 MiB) through cc1 -- see the CL-5 probe in usr/joey/joey.c. Note the gate
// runs it as PRINCIPAL_SYSTEM (joey's child), which is resource-EXEMPT, so that
// boot never consults a budget at all; the numbers below are what a real
// (non-exempt) user would be measured against. If these stop holding, the
// budget constant moved and LLVM-DESIGN.md
// section 7 needs re-deriving. NOTE the stressor FITS the default (64066 of
// 65536 = 97.8%); it is a REAL project TU (~2x+) that does not.
#define CL5_MEASURED_CC1_PEAK_PAGES 64066u

void test_resource_measured_compile_needs_raise(void) {
    struct Proc *p = res_make(A_REAL_USER);          // NOT exempt -- a real user

    // The measured stressor FITS the default -- but only just. 64066 of 65536
    // pages is 97.8% of the budget, i.e. 1470 pages (5.7 MiB) of headroom for a
    // 1959-BYTE source file. Asserted as a fact, and as a tripwire: if a future
    // change pushes this over, the default budget stopped covering even the
    // trivial case.
    _Static_assert(CL5_MEASURED_CC1_PEAK_PAGES < PROC_PAGE_MAX,
                   "the measured stressor is expected to fit the default budget");
    TEST_ASSERT(proc_page_charge(p, CL5_MEASURED_CC1_PEAK_PAGES),
                "the measured cc1 peak fits the default budget (97.8% of it)");
    TEST_EXPECT_EQ(p->page_peak, CL5_MEASURED_CC1_PEAK_PAGES,
                   "the high-water recorded the measured peak");
    proc_page_uncharge(p, CL5_MEASURED_CC1_PEAK_PAGES);

    // A REAL project TU does not. The stressor is 1959 bytes; DAGCombiner.cpp
    // is 1.2 MB and measured 735 MiB RSS on the host, which scales to ~500-650
    // MiB of device anon at the 0.70 anon fraction measured between the two
    // device data points. 2x the stressor is the CONSERVATIVE bottom of that
    // range and already exceeds the default -- so the collision is real without
    // needing the projection to be precise.
    u32 real_tu = CL5_MEASURED_CC1_PEAK_PAGES * 2u;   // ~500 MiB
    TEST_ASSERT(real_tu > PROC_PAGE_MAX, "a real TU exceeds the default budget");
    TEST_ASSERT(!proc_page_charge(p, real_tu),
                "a real project TU does NOT fit the default 256 MiB budget");
    TEST_EXPECT_EQ(p->page_count, 0u, "the refused charge committed nothing");

    // ...and DOES fit once the budget is raised. That is the whole mechanism.
    p->page_budget = real_tu + 32768u;               // +128 MiB of headroom
    TEST_ASSERT(proc_page_charge(p, real_tu),
                "a real project TU fits a raised budget");
    TEST_EXPECT_EQ(p->page_count, real_tu, "the raised charge committed in full");

    // The TCB is exempt regardless of budget -- which is exactly why the clade
    // gate's own clang++ (spawned by joey, PRINCIPAL_SYSTEM) does not hit this.
    struct Proc *sys = res_make((u32)PRINCIPAL_SYSTEM);
    sys->page_budget = 1u;                            // absurdly small
    TEST_ASSERT(proc_page_charge(sys, CL5_MEASURED_CC1_PEAK_PAGES),
                "PRINCIPAL_SYSTEM ignores the budget entirely");

    res_drop(p); res_drop(sys);
}

void test_resource_thread_cap_ok(void) {
    struct Proc *p = res_make(A_REAL_USER);   // non-exempt

    p->thread_count = PROC_THREAD_MAX - 1;
    TEST_ASSERT(proc_thread_cap_ok(p), "below the thread cap -> ok");
    p->thread_count = PROC_THREAD_MAX;
    TEST_ASSERT(!proc_thread_cap_ok(p), "at the thread cap -> refused");
    p->thread_count = PROC_THREAD_MAX + 5;
    TEST_ASSERT(!proc_thread_cap_ok(p), "over the thread cap -> refused");

    struct Proc *sys = res_make((u32)PRINCIPAL_SYSTEM);
    sys->thread_count = PROC_THREAD_MAX + 100;
    TEST_ASSERT(proc_thread_cap_ok(sys), "exempt is unbounded by the thread cap");

    TEST_ASSERT(!proc_thread_cap_ok(NULL), "NULL -> refused (fail-closed)");

    p->thread_count = 0; sys->thread_count = 0;   // reset for proc_free's gate
    res_drop(p); res_drop(sys);
}

void test_resource_child_cap_ok(void) {
    struct Proc *p = res_make(A_REAL_USER);   // non-exempt

    p->child_count = PROC_CHILD_MAX - 1;
    TEST_ASSERT(proc_child_cap_ok(p), "below the child cap -> ok");
    p->child_count = PROC_CHILD_MAX;
    TEST_ASSERT(!proc_child_cap_ok(p), "at the child cap -> refused");

    struct Proc *sys = res_make((u32)PRINCIPAL_SYSTEM);
    sys->child_count = PROC_CHILD_MAX + 100;
    TEST_ASSERT(proc_child_cap_ok(sys), "exempt is unbounded by the child cap");

    TEST_ASSERT(!proc_child_cap_ok(NULL), "NULL -> refused (fail-closed)");

    p->child_count = 0; sys->child_count = 0;
    res_drop(p); res_drop(sys);
}

void test_resource_child_count_tracks_list(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 base = __atomic_load_n(&kproc()->child_count, __ATOMIC_ACQUIRE);
    proc_test_link(p);    // -> proc_link_child(kproc(), p): child_count++
    TEST_EXPECT_EQ(__atomic_load_n(&kproc()->child_count, __ATOMIC_ACQUIRE), base + 1u,
                   "link bumps child_count");
    proc_test_unlink(p);  // manual splice + child_count--
    TEST_EXPECT_EQ(__atomic_load_n(&kproc()->child_count, __ATOMIC_ACQUIRE), base,
                   "unlink restores child_count");

    res_drop(p);
}

// Exercises the PRODUCTION child_count maintenance through a real rfork + reap
// (proc_link_child ++ at rfork, proc_unlink_child -- at the reap) -- not just the
// test-only proc_test_unlink. The harness runs as a kproc thread, so the child
// is kproc's; kproc is exempt (the cap never fires), but the counter is
// maintained regardless. wait_pid_for(pid) reaps exactly our child, so a stray
// zombie from another test cannot perturb the delta.
void test_resource_child_count_rfork_reap(void) {
    struct Proc *kp = kproc();
    u32 base = __atomic_load_n(&kp->child_count, __ATOMIC_ACQUIRE);

    int pid = rfork(RFPROC, res_child_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");
    TEST_EXPECT_EQ(__atomic_load_n(&kp->child_count, __ATOMIC_ACQUIRE), base + 1u,
                   "rfork bumps kproc child_count (proc_link_child)");

    int st = -1;
    int reaped = wait_pid_for(pid, 0, &st);
    TEST_EXPECT_EQ(reaped, pid, "reap the rforked child by pid");
    TEST_EXPECT_EQ(__atomic_load_n(&kp->child_count, __ATOMIC_ACQUIRE), base,
                   "reap restores child_count (proc_unlink_child)");
}

void test_resource_page_cap_attach_enforced(void) {
    struct Proc *p = res_make((u32)PRINCIPAL_INVALID);   // non-exempt (bare default)

    // Pre-charge one below the cap so the boundary is exercised WITHOUT a
    // 256-MiB allocation. The 2-page attach would push to cap+1 -> refused at
    // the cap check, which precedes burrow_create_anon (nothing allocated).
    __atomic_store_n(&p->page_count, PROC_PAGE_MAX - 1u, __ATOMIC_RELEASE);
    s64 over = sys_burrow_attach_for_proc(p, 2u * PAGE_SIZE);
    TEST_EXPECT_EQ(over, (s64)(-T_E_NOMEM), "over-cap attach -> -ENOMEM");
    TEST_EXPECT_EQ(__atomic_load_n(&p->page_count, __ATOMIC_ACQUIRE), PROC_PAGE_MAX - 1u,
                   "an over-cap attach charges/allocates nothing");

    // A 1-page attach fits exactly (page_count + 1 == cap) -> succeeds + charges.
    s64 fit = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(fit >= 0, "the boundary-fitting attach succeeds");
    TEST_EXPECT_EQ(__atomic_load_n(&p->page_count, __ATOMIC_ACQUIRE), PROC_PAGE_MAX,
                   "the fitting attach charged 1 page");

    // Detach uncharges exactly.
    s64 d = sys_burrow_detach_for_proc(p, (u64)fit, PAGE_SIZE);
    TEST_EXPECT_EQ(d, 0L, "detach ok");
    TEST_EXPECT_EQ(__atomic_load_n(&p->page_count, __ATOMIC_ACQUIRE), PROC_PAGE_MAX - 1u,
                   "detach uncharged 1 page");

    res_drop(p);

    // An exempt Proc's attach bypasses the cap in the real syscall path.
    struct Proc *sys = res_make((u32)PRINCIPAL_SYSTEM);
    __atomic_store_n(&sys->page_count, PROC_PAGE_MAX - 1u, __ATOMIC_RELEASE);
    s64 ex = sys_burrow_attach_for_proc(sys, 2u * PAGE_SIZE);
    TEST_ASSERT(ex >= 0, "exempt attach past the cap succeeds");
    TEST_EXPECT_EQ(__atomic_load_n(&sys->page_count, __ATOMIC_ACQUIRE), PROC_PAGE_MAX + 1u,
                   "exempt charged past the cap");
    (void)sys_burrow_detach_for_proc(sys, (u64)ex, 2u * PAGE_SIZE);
    res_drop(sys);
}

// I-32 FOURTH axis (overcommit, ARCH section 6.5): proc_vma_charge bounds live VMAs
// at PROC_VMA_MAX -- the DoS a free SYS_BURROW_ATTACH_LAZY reservation would open.
// Mirrors the page-charge cap test (no 65536 real VMAs; pre-set vma_count near the
// boundary). proc_vma_charge requires p->vma_lock in production for EXACTNESS under
// contention; this single-threaded test calls it directly (the __atomic ops are
// coherent without the lock when no peer races).
void test_resource_vma_cap(void) {
    struct Proc *p = res_make(A_REAL_USER);   // non-exempt

    // Boundary: charge at MAX-1 succeeds (-> MAX), the next is refused (no over-cap).
    __atomic_store_n(&p->vma_count, PROC_VMA_MAX - 1u, __ATOMIC_RELEASE);
    TEST_ASSERT(proc_vma_charge(p),  "charge at MAX-1 succeeds (-> MAX)");
    TEST_EXPECT_EQ(p->vma_count, PROC_VMA_MAX, "vma_count == cap after the boundary charge");
    TEST_ASSERT(!proc_vma_charge(p), "charge at the cap is refused");
    TEST_EXPECT_EQ(p->vma_count, PROC_VMA_MAX, "a refused charge charges nothing");

    // Uncharge re-opens a slot; charge then succeeds again.
    proc_vma_uncharge(p);
    TEST_EXPECT_EQ(p->vma_count, PROC_VMA_MAX - 1u, "uncharge re-opens a slot");
    TEST_ASSERT(proc_vma_charge(p), "charge succeeds again after uncharge");

    // Over-uncharge clamps at 0 (no underflow).
    __atomic_store_n(&p->vma_count, 0u, __ATOMIC_RELEASE);
    proc_vma_uncharge(p);
    TEST_EXPECT_EQ(p->vma_count, 0u, "over-uncharge clamps at 0 (no underflow)");

    // Saturation guard: a charge on a maxed counter is refused (no wrap).
    __atomic_store_n(&p->vma_count, 0xFFFFFFFFu, __ATOMIC_RELEASE);
    TEST_ASSERT(!proc_vma_charge(p), "saturated counter refuses a charge");
    __atomic_store_n(&p->vma_count, 0u, __ATOMIC_RELEASE);

    // Exempt Procs (PRINCIPAL_SYSTEM) bypass the cap; the count is still maintained.
    struct Proc *sys = res_make((u32)PRINCIPAL_SYSTEM);
    __atomic_store_n(&sys->vma_count, PROC_VMA_MAX, __ATOMIC_RELEASE);
    TEST_ASSERT(proc_vma_charge(sys), "exempt charges past the cap");
    TEST_EXPECT_EQ(sys->vma_count, PROC_VMA_MAX + 1u, "exempt is unbounded by the cap");
    __atomic_store_n(&sys->vma_count, 0u, __ATOMIC_RELEASE);

    res_drop(p); res_drop(sys);
}
