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

#include "test.h"

#include <thylacine/addrspace.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>

void test_addrspace_alloc_shape(void);
void test_addrspace_refcount(void);
void test_addrspace_kproc_has_none(void);
void test_addrspace_charge_helpers_refuse_without_as(void);

void test_addrspace_alloc_shape(void) {
    struct AddrSpace *as = addrspace_alloc();
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
    struct AddrSpace *as = addrspace_alloc();
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
