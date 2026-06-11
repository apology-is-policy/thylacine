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
void test_resource_page_cap_attach_enforced(void);

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
