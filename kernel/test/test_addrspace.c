// LINEAGE L-1: struct AddrSpace lifecycle tests.
//
//   addrspace.alloc_shape
//     A fresh AddrSpace has ref 1, a real page table, an empty VMA list, an
//     unassigned ASID context and zeroed I-32 axes.
//
//   addrspace.refcount
//     ref/unref balance: the object survives while a second reference is held
//     and frees only on the last drop. (At L-1 nothing in the tree shares an
//     address space -- this pins the mechanism L-3's RFMEM and L-5's fork use.)
//
//   addrspace.kproc_has_none
//     kproc has NO address space. This is the equivalence the whole refactor
//     rests on: `as == NULL` replaced the old `pgtable_root == 0` test, so if
//     kproc ever acquired one, every kernel-Proc gate in the tree would start
//     answering the wrong question.
//
//   addrspace.charge_helpers_refuse_without_as
//     The I-32 charge helpers refuse (and the uncharges no-op) on a Proc with
//     no address space, instead of dereferencing NULL. The reachable readers of
//     this property are the Proc-table walkers -- `/ctl/procs` and `/proc/<pid>`
//     walk a tree whose ROOT is kproc -- and the boot itself proves it: removing
//     the guard in devctl.c makes the kernel extinct during boot with an
//     unhandled translation fault at 0x20, which is page_count's offset in
//     struct AddrSpace.

// LINEAGE L-3 adds:
//
//   addrspace.share_drains_at_last_ref
//     THE I-44 claim, and the reason the drain moved out of proc_free: a
//     non-final unref must leave the mappings alone, and the final one must
//     take them. Draining at a Proc's death and draining at the last reference
//     were the same event only while nothing shared.
//
//   addrspace.proc_alloc_in_shares
//     proc_alloc_in(as, PROC_PAGE_MAX) gives the Proc that exact space and takes a reference,
//     so the Proc's later death drops a reference rather than the space. This
//     is the mechanism rfork(RFPROC|RFMEM) is built from.
//
//   proc.rfork_rfmem_refuses_without_addrspace
//     RFMEM from a Proc with no address space is refused rather than silently
//     downgraded to a private one -- driven through the real rfork.
//
// NOT covered here, deliberately: a SUCCESSFUL rfork(RFPROC|RFMEM) end to end.
// The only Proc a kernel test runs on is kproc, which has no address space by
// construction (addrspace.kproc_has_none above), and lending it one for the
// duration would make every `as == NULL` kernel-Proc gate in the tree answer
// the wrong question while a real child thread was running against it. So the
// mechanism is pinned at the two layers a kernel test CAN reach, and the
// end-to-end proof lands with the EL0 surface at L-3c -- the same split L-2a
// made, where the detached build was unit-tested and the swap needed
// /exec-probe.

#include "test.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/vma.h>

void test_addrspace_alloc_shape(void);
void test_addrspace_refcount(void);
void test_addrspace_kproc_has_none(void);
void test_addrspace_charge_helpers_refuse_without_as(void);
void test_addrspace_share_drains_at_last_ref(void);
void test_addrspace_proc_alloc_in_shares(void);
void test_proc_rfork_rfmem_refuses_without_addrspace(void);

void test_addrspace_alloc_shape(void) {
    struct AddrSpace *as = addrspace_alloc(PROC_PAGE_MAX);
    TEST_ASSERT(as != NULL, "addrspace_alloc returned NULL");
    TEST_ASSERT(__atomic_load_n(&as->ref, __ATOMIC_ACQUIRE) == 1,
                "a fresh AddrSpace starts at ref 1");
    TEST_ASSERT(as->pgtable_root != 0, "a fresh AddrSpace has a page table");
    TEST_ASSERT(as->vmas == NULL, "a fresh AddrSpace has an empty VMA list");
    TEST_ASSERT(as->context_id == 0,
                "context_id starts 0 == never assigned (the rolling allocator "
                "stamps it at the first context switch, not at create)");
    TEST_ASSERT(as->page_count == 0 && as->vma_count == 0 &&
                as->shared_map_pages == 0,
                "the three I-32 axes start zeroed");
    addrspace_unref(as);
}

void test_addrspace_refcount(void) {
    struct AddrSpace *as = addrspace_alloc(PROC_PAGE_MAX);
    TEST_ASSERT(as != NULL, "addrspace_alloc returned NULL");

    addrspace_ref(as);
    TEST_ASSERT(__atomic_load_n(&as->ref, __ATOMIC_ACQUIRE) == 2,
                "a second reference took the count to 2");

    // The first drop must NOT free -- read a field afterwards to prove the
    // object is still live (a premature free would trip SLUB's poison or the
    // magic check on the next use).
    addrspace_unref(as);
    TEST_ASSERT(__atomic_load_n(&as->ref, __ATOMIC_ACQUIRE) == 1,
                "the non-final unref left the object live at ref 1");
    TEST_ASSERT(as->pgtable_root != 0,
                "the page table survives a non-final unref");

    addrspace_unref(as);   // last drop: destroys the page table + frees

    // NULL-safe by contract: a kernel-only Proc, or a proc_alloc rollback that
    // failed before addrspace_alloc ran, reaches proc_free with as == NULL.
    addrspace_unref(NULL);
}

void test_addrspace_share_drains_at_last_ref(void) {
    struct AddrSpace *as = addrspace_alloc(PROC_PAGE_MAX);
    TEST_ASSERT(as != NULL, "addrspace_alloc returned NULL");

    struct Burrow *b = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(b != NULL, "burrow_create_anon failed");
    int mapped_before = burrow_mapping_count(b);

    // Map into the address space directly. exempt=true: there is no Proc here
    // to ask for an I-32 verdict, and the cap is not what this test is about.
    int rc = burrow_map_in(as, true, b, 0x40000000ull, PAGE_SIZE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map_in should map into a bare AddrSpace");
    TEST_ASSERT(as->vmas != NULL, "precondition: the map installed a VMA");
    TEST_EXPECT_EQ(burrow_mapping_count(b), mapped_before + 1,
                   "precondition: the map took a Burrow mapping ref");

    // A second sharer -- what rfork(RFPROC|RFMEM) produces.
    addrspace_ref(as);
    TEST_EXPECT_EQ(addrspace_ref_count(as), 2, "two Procs now hold this space");

    // The first sharer dies. This MUST NOT touch the mappings: the survivor is
    // still translating through them. Pre-L-3, proc_free drained here
    // unconditionally and this is the assertion that would have failed.
    addrspace_unref(as);
    TEST_EXPECT_EQ(addrspace_ref_count(as), 1,
                   "the non-final unref left one holder");
    TEST_ASSERT(as->vmas != NULL,
                "a non-final unref must NOT drain -- the surviving sharer is "
                "still using these mappings");
    TEST_EXPECT_EQ(burrow_mapping_count(b), mapped_before + 1,
                   "and the Burrow mapping ref survives with them");

    // The last sharer dies: now the mappings are nobody's, and go.
    addrspace_unref(as);
    TEST_EXPECT_EQ(burrow_mapping_count(b), mapped_before,
                   "the LAST unref drained the VMA list and released the "
                   "Burrow mapping ref");

    burrow_unref(b);
}

void test_addrspace_proc_alloc_in_shares(void) {
    struct AddrSpace *as = addrspace_alloc(PROC_PAGE_MAX);
    TEST_ASSERT(as != NULL, "addrspace_alloc returned NULL");

    struct Proc *p = proc_alloc_in(as, PROC_PAGE_MAX);
    TEST_ASSERT(p != NULL, "proc_alloc_in returned NULL");
    TEST_ASSERT(p->as == as,
                "proc_alloc_in must adopt the space it was handed, not a copy "
                "of it -- sharing is pointer identity, not equal contents");
    TEST_EXPECT_EQ(addrspace_ref_count(as), 2,
                   "proc_alloc_in took a reference for the new Proc");

    // The Proc dies; the space does not.
    p->state = 2;   // PROC_STATE_ZOMBIE
    proc_free(p);
    TEST_EXPECT_EQ(addrspace_ref_count(as), 1,
                   "the Proc's death dropped ITS reference, leaving the space "
                   "alive for the holder that is still here");

    addrspace_unref(as);

    // The counterpart: the NULL form is the old behaviour exactly -- a fresh
    // space of its own, which is what every pre-L-3 caller wants and gets.
    struct Proc *q = proc_alloc_in(NULL, PROC_PAGE_MAX);
    TEST_ASSERT(q != NULL, "proc_alloc_in(NULL, PROC_PAGE_MAX) returned NULL");
    TEST_ASSERT(q->as != NULL, "proc_alloc_in(NULL, PROC_PAGE_MAX) allocates a fresh space");
    TEST_EXPECT_EQ(addrspace_ref_count(q->as), 1,
                   "a fresh space is held by exactly its one Proc");
    q->state = 2;
    proc_free(q);
}

// A child that exits immediately. Only reached if the guard under test is
// GONE -- which is the point: a regression must leave something reapable
// rather than wedging the suite.
static void rfmem_spawn_thunk(void *arg) { (void)arg; exits("ok"); }

void test_proc_rfork_rfmem_refuses_without_addrspace(void) {
    struct Proc *me = kproc();
    TEST_ASSERT(me != NULL, "kproc()");
    TEST_ASSERT(me->as == NULL,
                "precondition: the Proc a kernel test runs on has no address "
                "space, which is exactly the case under test");

    // Control: RFPROC alone still spawns. Without this leg the test below would
    // also pass if rfork had broken outright, which is the shape where a green
    // assertion means nothing.
    int cpid = rfork(RFPROC, rfmem_spawn_thunk, NULL);
    TEST_ASSERT(cpid > 0, "RFPROC alone still spawns a child");
    int cst = -1;
    TEST_EXPECT_EQ(wait_pid_for(cpid, 0, &cst), cpid, "reap the control child");

    // Driven through the REAL rfork, so this pins the entry point rather than a
    // helper. The refusal lands before proc_alloc, so a pass leaves nothing to
    // reap; a REGRESSION spawns a child that shares nothing, which the pid
    // assertion below catches (and the thunk above lets exit cleanly).
    int pid = rfork(RFPROC | RFMEM, rfmem_spawn_thunk, NULL);

    // Reap BEFORE asserting, not after. TEST_ASSERT *returns* on failure, so a
    // cleanup placed below the assertion never runs on precisely the path that
    // needs it: the child would leak, an unrelated later test's reap-any would
    // collect it, and the visible symptom would be "wait_pid pid mismatch"
    // somewhere else entirely. (Found by the revert probe for this very guard,
    // which is what a probe is for -- it exercises the failure path that the
    // green run by definition never takes.)
    if (pid > 0) {
        int st = -1;
        (void)wait_pid_for(pid, 0, &st);
    }

    TEST_EXPECT_EQ(pid, -1,
                   "RFMEM from a Proc with no address space must be REFUSED, "
                   "not silently downgraded to a private space -- otherwise "
                   "'the flag worked' and 'the flag was ignored' look alike");
}

void test_addrspace_kproc_has_none(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc() returned NULL");
    TEST_ASSERT(kp->as == NULL,
                "kproc must have NO address space -- `as == NULL` IS the "
                "kernel-only test that replaced `pgtable_root == 0`");
}

void test_addrspace_charge_helpers_refuse_without_as(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp->as == NULL, "precondition: kproc has no address space");

    // Refuse rather than deref. Refusing fails CLOSED -- a caller that somehow
    // reached here rolls back instead of charging an address space that does
    // not exist.
    TEST_ASSERT(proc_page_charge(kp, 1) == false,
                "proc_page_charge must refuse a Proc with no address space");
    TEST_ASSERT(proc_vma_charge(kp) == false,
                "proc_vma_charge must refuse a Proc with no address space");
    TEST_ASSERT(proc_shared_map_charge(kp, 1) == false,
                "proc_shared_map_charge must refuse a Proc with no address space");

    // The uncharges are void; reaching them without a fault is the assertion.
    proc_page_uncharge(kp, 1);
    proc_vma_uncharge(kp);
    proc_shared_map_uncharge(kp, 1);
}
