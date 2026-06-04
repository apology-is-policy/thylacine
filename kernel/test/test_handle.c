// Handle table tests (P2-Fc).
//
// Six tests:
//
//   handles.alloc_close_smoke
//     Allocate a fresh Proc; alloc several handles of different kinds;
//     verify counts increment; close them all; verify counts back to 0.
//     Cumulative allocated/freed counters tracked.
//
//   handles.rights_monotonic
//     Alloc a parent handle with rights={READ, WRITE}; dup with subset
//     rights={READ} succeeds; dup with elevated rights={READ, MAP}
//     returns -1 (would fabricate MAP bit). Models impl-side enforcement
//     of the spec's RightsCeiling invariant.
//
//   handles.dup_lifecycle
//     Dup a handle; close the parent; verify dup remains valid.
//     Dup again; close the child; verify the original remains valid.
//     Independent close ordering.
//
//   handles.full_table_oom
//     Alloc handles up to PROC_HANDLE_MAX; verify slot indices 0..MAX-1.
//     Alloc one more; expect -1 (table full). Close one mid-range; alloc
//     again; verify the freed slot is reused.
//
//   handles.kind_classifiers
//     Truth table: kobj_kind_is_transferable / kobj_kind_is_hw /
//     kobj_kind_is_srv for every enum value (KOBJ_INVALID, PROCESS,
//     THREAD, BURROW, SPOOR, MMIO, IRQ, DMA, INTERRUPT, SRV). Pins the
//     spec's TxKObjs / HwKObjs / SrvKObjs partition.
//
//   handles.srv_kind
//     KObj_Srv (P5-corvus-srv): classifies into the srv partition
//     (non-transferable, non-hardware); a KObj_Srv handle allocs +
//     closes normally but cannot be dup'd (NoSrvDup — exactly one /srv
//     connection Spoor per Proc, CORVUS-DESIGN.md §6.2).
//
// Maps to specs/handles.tla state invariants:
//   - RightsCeiling: rights_monotonic
//   - HwHandlesAtOrigin / TransferableTypes: kind_classifiers (the runtime
//     side of the structurally-prevented bug class is the classifier
//     truth table)
//   - HandleAlloc / HandleClose / HandleDup mechanics: alloc_close_smoke
//     + dup_lifecycle + full_table_oom
//   - SrvHandlesAtOrigin: srv_kind (the runtime side of KObj_Srv non-
//     transferability is the classifier truth table + the NoSrvDup
//     rejection in handle_dup)

#include "test.h"

#include <thylacine/handle.h>
#include <thylacine/territory.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>

void test_handles_alloc_close_smoke(void);
void test_handles_rights_monotonic(void);
void test_handles_dup_lifecycle(void);
void test_handles_full_table_oom(void);
void test_handles_kind_classifiers(void);
void test_handles_srv_kind(void);

// Shared test helper: allocate a Proc, exercise it, then transition to
// ZOMBIE + free. proc_alloc has already allocated a fresh empty handle
// table; proc_free will release it (closing any straggler handles).
static struct Proc *test_proc_make(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    // proc_alloc leaves territory = NULL; proc_free's territory_unref(NULL) is a
    // no-op, so that's fine for tests that don't exercise territory.
    return p;
}

static void test_proc_drop(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_handles_alloc_close_smoke(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    TEST_EXPECT_EQ(handle_table_count(p->handles), 0,
        "fresh Proc must have 0 handles");

    u64 alloc_before = handle_total_allocated();
    u64 freed_before = handle_total_freed();

    // Alloc a Process handle with READ + TRANSFER rights.
    hidx_t h1 = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ | RIGHT_TRANSFER, p);
    TEST_ASSERT(h1 >= 0, "handle_alloc returned -1");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 1, "count should be 1");

    // Alloc a Thread handle with READ rights.
    hidx_t h2 = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h2 >= 0, "handle_alloc(THREAD) returned -1");
    TEST_ASSERT(h2 != h1, "second alloc must yield distinct slot");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 2, "count should be 2");

    // handle_get returns the right kind + rights.
    struct Handle got1;
    TEST_ASSERT(handle_get(p, h1, &got1) == 0, "handle_get(h1) returned -1");
    TEST_EXPECT_EQ((int)got1.kind, (int)KOBJ_PROCESS, "h1 kind mismatch");
    TEST_EXPECT_EQ(got1.rights, (rights_t)(RIGHT_READ | RIGHT_TRANSFER),
        "h1 rights mismatch");
    handle_put(&got1);

    // Reject KOBJ_INVALID at alloc.
    hidx_t bad_kind = handle_alloc(p, KOBJ_INVALID, RIGHT_READ, NULL);
    TEST_EXPECT_EQ(bad_kind, -1, "alloc with KOBJ_INVALID must fail");

    // Reject empty rights.
    hidx_t bad_rights = handle_alloc(p, KOBJ_THREAD, RIGHT_NONE, NULL);
    TEST_EXPECT_EQ(bad_rights, -1, "alloc with RIGHT_NONE must fail");

    // Reject rights bits outside RIGHT_ALL.
    hidx_t bad_bits = handle_alloc(p, KOBJ_THREAD, 0xff00u, NULL);
    TEST_EXPECT_EQ(bad_bits, -1, "alloc with out-of-range rights must fail");

    // Close h1.
    TEST_EXPECT_EQ(handle_close(p, h1), 0, "handle_close(h1)");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 1, "count should be 1");
    struct Handle closed_tmp;
    TEST_EXPECT_EQ(handle_get(p, h1, &closed_tmp), -1,
        "handle_get on closed slot must return -1");

    // Double-close returns -1.
    TEST_EXPECT_EQ(handle_close(p, h1), -1, "double-close must return -1");

    // Out-of-range close returns -1.
    TEST_EXPECT_EQ(handle_close(p, -1), -1, "close of -1 must return -1");
    TEST_EXPECT_EQ(handle_close(p, PROC_HANDLE_MAX), -1,
        "close of MAX must return -1");

    // Close h2.
    TEST_EXPECT_EQ(handle_close(p, h2), 0, "handle_close(h2)");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 0, "count should be 0");

    // Cumulative counters incremented.
    u64 alloc_after = handle_total_allocated();
    u64 freed_after = handle_total_freed();
    TEST_EXPECT_EQ(alloc_after - alloc_before, (u64)2,
        "two successful allocs must increment the counter by 2");
    TEST_EXPECT_EQ(freed_after - freed_before, (u64)2,
        "two closes must increment the freed counter by 2");

    test_proc_drop(p);
}

void test_handles_rights_monotonic(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    // Parent has READ + WRITE.
    hidx_t parent = handle_alloc(p, KOBJ_BURROW, RIGHT_READ | RIGHT_WRITE, NULL);
    TEST_ASSERT(parent >= 0, "parent alloc failed");

    // Dup with subset rights = READ. Succeeds.
    hidx_t child_read = handle_dup(p, parent, RIGHT_READ);
    TEST_ASSERT(child_read >= 0, "dup with subset rights must succeed");
    struct Handle got;
    TEST_ASSERT(handle_get(p, child_read, &got) == 0, "child_read handle_get -1");
    TEST_EXPECT_EQ(got.rights, (rights_t)RIGHT_READ,
        "child_read rights must be exactly READ");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_BURROW,
        "child_read kind preserved from parent");
    handle_put(&got);   // #844: KOBJ_BURROW -> the snapshot held a burrow_ref

    // Dup with same rights = READ + WRITE. Succeeds (subset of self is self).
    hidx_t child_full = handle_dup(p, parent, RIGHT_READ | RIGHT_WRITE);
    TEST_ASSERT(child_full >= 0, "dup with same rights must succeed");

    // Dup with elevated rights = READ + WRITE + MAP. Fails: MAP not in
    // parent's rights. The impl-side enforcement of RightsCeiling
    // invariant: BuggyDupElevate produces a counterexample at the spec
    // level; the impl rejects at runtime.
    hidx_t bad_dup = handle_dup(p, parent, RIGHT_READ | RIGHT_WRITE | RIGHT_MAP);
    TEST_EXPECT_EQ(bad_dup, -1,
        "dup with elevated rights must return -1 (elevation rejected)");

    // Dup with completely different rights bits = MAP. Also rejected
    // (MAP not in parent's rights).
    hidx_t bad_dup_2 = handle_dup(p, parent, RIGHT_MAP);
    TEST_EXPECT_EQ(bad_dup_2, -1,
        "dup with disjoint rights must return -1 (not subset)");

    // Dup with empty rights returns -1.
    hidx_t bad_empty = handle_dup(p, parent, RIGHT_NONE);
    TEST_EXPECT_EQ(bad_empty, -1, "dup with RIGHT_NONE must fail");

    // Dup of an empty slot returns -1.
    hidx_t bad_h = handle_dup(p, 99, RIGHT_READ);
    TEST_EXPECT_EQ(bad_h, -1, "dup of out-of-range slot must fail");

    // Cleanup.
    handle_close(p, parent);
    handle_close(p, child_read);
    handle_close(p, child_full);

    test_proc_drop(p);
}

void test_handles_dup_lifecycle(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t parent = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ | RIGHT_TRANSFER, p);
    TEST_ASSERT(parent >= 0, "parent alloc failed");

    hidx_t dup = handle_dup(p, parent, RIGHT_READ);
    TEST_ASSERT(dup >= 0, "dup failed");
    TEST_ASSERT(dup != parent, "dup must yield a distinct slot");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 2, "count should be 2");

    // Close parent. Dup should still be valid.
    TEST_EXPECT_EQ(handle_close(p, parent), 0, "close parent");
    struct Handle parent_tmp;
    TEST_EXPECT_EQ(handle_get(p, parent, &parent_tmp), -1, "parent slot now empty");

    struct Handle dup_got;
    TEST_ASSERT(handle_get(p, dup, &dup_got) == 0, "dup must remain valid after parent close");
    TEST_EXPECT_EQ(dup_got.rights, (rights_t)RIGHT_READ,
        "dup rights unchanged after parent close");
    handle_put(&dup_got);
    TEST_EXPECT_EQ(handle_table_count(p->handles), 1, "count should be 1");

    // Now dup again from the surviving handle, then close it; original
    // dup remains.
    hidx_t dup2 = handle_dup(p, dup, RIGHT_READ);
    TEST_ASSERT(dup2 >= 0, "second dup failed");
    TEST_EXPECT_EQ(handle_close(p, dup2), 0, "close dup2");
    struct Handle dup_tmp;
    TEST_ASSERT(handle_get(p, dup, &dup_tmp) == 0,
        "first dup must remain valid after second dup is closed");
    handle_put(&dup_tmp);

    // Cleanup.
    handle_close(p, dup);

    test_proc_drop(p);
}

void test_handles_full_table_oom(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t slots[PROC_HANDLE_MAX];
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        slots[i] = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
        TEST_ASSERT(slots[i] >= 0, "alloc within table capacity must succeed");
    }
    TEST_EXPECT_EQ(handle_table_count(p->handles), PROC_HANDLE_MAX,
        "count should be PROC_HANDLE_MAX");

    // One more — table full. Returns -1.
    hidx_t overflow = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_EXPECT_EQ(overflow, -1, "alloc past PROC_HANDLE_MAX must return -1");

    // Close a mid-range slot; alloc again; verify the freed slot is
    // reused (handle_alloc scans linearly).
    int mid = PROC_HANDLE_MAX / 2;
    TEST_EXPECT_EQ(handle_close(p, slots[mid]), 0, "close mid slot");
    hidx_t reused = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_EXPECT_EQ(reused, slots[mid],
        "alloc after partial close must reuse the freed slot");

    // Cleanup all handles.
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (i == mid) continue;     // closed above; reuse alloc'd here
        TEST_EXPECT_EQ(handle_close(p, slots[i]), 0, "close slot[i]");
    }
    TEST_EXPECT_EQ(handle_close(p, reused), 0, "close reused slot");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 0, "all closed");

    test_proc_drop(p);
}

void test_handles_kind_classifiers(void) {
    // Transferable: PROCESS, THREAD, BURROW, CHAN.
    TEST_ASSERT(kobj_kind_is_transferable(KOBJ_PROCESS),
        "PROCESS must be transferable");
    TEST_ASSERT(kobj_kind_is_transferable(KOBJ_THREAD),
        "THREAD must be transferable");
    TEST_ASSERT(kobj_kind_is_transferable(KOBJ_BURROW),
        "BURROW must be transferable");
    TEST_ASSERT(kobj_kind_is_transferable(KOBJ_SPOOR),
        "CHAN must be transferable");

    // Hardware: MMIO, IRQ, DMA, INTERRUPT.
    TEST_ASSERT(kobj_kind_is_hw(KOBJ_MMIO),       "MMIO must be hw");
    TEST_ASSERT(kobj_kind_is_hw(KOBJ_IRQ),        "IRQ must be hw");
    TEST_ASSERT(kobj_kind_is_hw(KOBJ_DMA),        "DMA must be hw");
    TEST_ASSERT(kobj_kind_is_hw(KOBJ_INTERRUPT),  "INTERRUPT must be hw");

    // Srv: the /srv connection-Spoor partition (P5-corvus-srv) — a
    // third disjoint set, non-transferable AND non-hardware.
    TEST_ASSERT(kobj_kind_is_srv(KOBJ_SRV),       "SRV must classify as srv");
    TEST_ASSERT(!kobj_kind_is_transferable(KOBJ_SRV),
        "SRV must NOT be transferable");
    TEST_ASSERT(!kobj_kind_is_hw(KOBJ_SRV),       "SRV must NOT be hw");
    TEST_ASSERT(!kobj_kind_is_srv(KOBJ_PROCESS),  "PROCESS must NOT be srv");
    TEST_ASSERT(!kobj_kind_is_srv(KOBJ_SPOOR),    "SPOOR must NOT be srv");
    TEST_ASSERT(!kobj_kind_is_srv(KOBJ_MMIO),     "MMIO must NOT be srv");
    TEST_ASSERT(!kobj_kind_is_srv(KOBJ_INVALID),  "KOBJ_INVALID must NOT be srv");
    TEST_ASSERT(!kobj_kind_is_srv((enum kobj_kind)99),
        "out-of-range kind must NOT be srv");

    // Disjoint: nothing is both.
    TEST_ASSERT(!kobj_kind_is_hw(KOBJ_PROCESS),
        "PROCESS must NOT be hw");
    TEST_ASSERT(!kobj_kind_is_transferable(KOBJ_MMIO),
        "MMIO must NOT be transferable");

    // KOBJ_INVALID is neither.
    TEST_ASSERT(!kobj_kind_is_transferable(KOBJ_INVALID),
        "KOBJ_INVALID must NOT be transferable");
    TEST_ASSERT(!kobj_kind_is_hw(KOBJ_INVALID),
        "KOBJ_INVALID must NOT be hw");

    // Out-of-range value rejected by both classifiers (defensive).
    TEST_ASSERT(!kobj_kind_is_transferable((enum kobj_kind)99),
        "out-of-range kind must NOT be transferable");
    TEST_ASSERT(!kobj_kind_is_hw((enum kobj_kind)99),
        "out-of-range kind must NOT be hw");
}

// handles.srv_kind — KObj_Srv (P5-corvus-srv-impl-a1).
//
// KObj_Srv is the kobj kind for a /srv/<name> connection Spoor. It is
// non-transferable and non-hardware — specs/handles.tla's third
// partition, SrvKObjs. This test pins:
//   - classification: srv, not transferable, not hw;
//   - a KObj_Srv handle allocs, looks up, and closes like any kind;
//   - NoSrvDup: handle_dup of a KObj_Srv handle returns -1 — the
//     runtime side of SrvHandlesAtOrigin (one connection Spoor per
//     Proc; handles.tla's HandleDup precondition `h.kobj \in TxKObjs`).
void test_handles_srv_kind(void) {
    // Classification: KObj_Srv is in exactly the srv partition.
    TEST_ASSERT(kobj_kind_is_srv(KOBJ_SRV),
        "KOBJ_SRV must classify as srv");
    TEST_ASSERT(!kobj_kind_is_transferable(KOBJ_SRV),
        "KOBJ_SRV must NOT be transferable (SrvHandlesAtOrigin)");
    TEST_ASSERT(!kobj_kind_is_hw(KOBJ_SRV),
        "KOBJ_SRV is non-transferable but NOT hardware");

    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    // A KObj_Srv handle allocates + looks up like any kind. obj is
    // NULL here (the underlying /srv connection object lands in
    // -impl-a3); handle_alloc accepts NULL obj on test paths.
    hidx_t h = handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, NULL);
    TEST_ASSERT(h >= 0, "handle_alloc(KOBJ_SRV) must succeed");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 1, "count should be 1");

    struct Handle got;
    TEST_ASSERT(handle_get(p, h, &got) == 0, "handle_get(srv) returned -1");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_SRV, "srv handle kind mismatch");
    handle_put(&got);

    // NoSrvDup: dup of a KObj_Srv handle is rejected — exactly as a
    // hardware handle's dup is. A subset-rights request is still
    // rejected: the kind, not the rights, is what forbids the dup.
    hidx_t dup = handle_dup(p, h, RIGHT_READ);
    TEST_EXPECT_EQ(dup, -1,
        "handle_dup of a KObj_Srv handle must return -1 (NoSrvDup)");
    hidx_t dup_same = handle_dup(p, h, RIGHT_READ | RIGHT_WRITE);
    TEST_EXPECT_EQ(dup_same, -1,
        "handle_dup of a KObj_Srv handle must return -1 even at same rights");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 1,
        "a rejected dup must not consume a slot");

    // Close releases the slot cleanly.
    TEST_EXPECT_EQ(handle_close(p, h), 0, "handle_close(srv)");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 0, "srv handle closed");

    test_proc_drop(p);
}
