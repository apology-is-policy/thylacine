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

#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/territory.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

void test_handles_alloc_close_smoke(void);
void test_handles_rights_monotonic(void);
void test_handles_dup_lifecycle(void);
void test_handles_full_table_oom(void);
void test_handles_kind_classifiers(void);
void test_handles_srv_kind(void);
void test_handles_dup_lowest(void);
void test_handles_status_word(void);

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

// =============================================================================
// handle_replace (VIVARIUM V-5). The three gates that make this primitive
// auditable are the whole test: the index is preserved, rights come from the
// caller rather than the outgoing slot, and every kind but KOBJ_SPOOR is
// refused on BOTH sides.
// =============================================================================
void test_handles_replace(void);
void test_handles_replace(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    // A KOBJ_SPOOR slot with a NULL obj: handle_alloc permits NULL for test
    // paths, and handle_release_obj's Spoor arm is NULL-safe, so this exercises
    // the slot mechanics without needing a live Dev.
    hidx_t h = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, NULL);
    TEST_ASSERT(h >= 0, "handle_alloc(SPOOR) returned -1");
    u64 count_before = handle_table_count(p->handles);

    // THE INDEX IS PRESERVED. That is the entire reason the primitive exists --
    // the guest is holding this number across connect().
    TEST_ASSERT(handle_replace(p, h, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, NULL) == 0,
                "replace of a live Spoor slot succeeds");
    TEST_EXPECT_EQ(handle_table_count(p->handles), count_before,
        "replace does not change the number of live handles");

    // RIGHTS COME FROM THE CALLER, NOT THE OUTGOING SLOT. A slot that was
    // READ-only is now R|W because the caller said so -- and, more importantly,
    // the reverse direction narrows rather than OR-ing.
    struct Handle got;
    TEST_ASSERT(handle_get(p, h, &got) == 0, "handle_get after replace");
    TEST_EXPECT_EQ(got.rights, (u64)(RIGHT_READ | RIGHT_WRITE),
        "rights are the caller's, exactly");
    handle_put(&got);

    TEST_ASSERT(handle_replace(p, h, KOBJ_SPOOR, RIGHT_READ, NULL) == 0, "narrowing replace");
    TEST_ASSERT(handle_get(p, h, &got) == 0, "handle_get after narrowing");
    TEST_EXPECT_EQ(got.rights, (u64)RIGHT_READ,
        "rights NARROWED -- the outgoing R|W was discarded, never inherited or "
        "OR'd (an inherited right would be an I-6 monotonicity break)");
    handle_put(&got);

    // THE INCOMING KIND IS GATED. Every non-Spoor kind is refused, so no path
    // here can make a hardware handle appear at an arbitrary fd (I-5).
    TEST_ASSERT(handle_replace(p, h, KOBJ_MMIO, RIGHT_READ, NULL) < 0,
                "incoming KOBJ_MMIO refused (I-5)");
    TEST_ASSERT(handle_replace(p, h, KOBJ_BURROW, RIGHT_READ, NULL) < 0,
                "incoming KOBJ_BURROW refused");
    TEST_ASSERT(handle_replace(p, h, KOBJ_PROCESS, RIGHT_READ, NULL) < 0,
                "incoming KOBJ_PROCESS refused -- the gate is an ALLOW-list of "
                "one kind, not a deny-list of hardware");

    // The refusals left the slot exactly as it was.
    TEST_ASSERT(handle_get(p, h, &got) == 0, "slot survives a refused replace");
    TEST_EXPECT_EQ(got.kind, KOBJ_SPOOR, "kind unchanged by refusal");
    TEST_EXPECT_EQ(got.rights, (u64)RIGHT_READ, "rights unchanged by refusal");
    handle_put(&got);

    // THE OUTGOING KIND IS GATED TOO. A non-Spoor slot cannot be replaced --
    // quietly dropping a hardware handle out of a live fd is a lifetime event
    // I-5 gives no path for.
    hidx_t hp = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, p);
    TEST_ASSERT(hp >= 0, "handle_alloc(PROCESS) returned -1");
    TEST_ASSERT(handle_replace(p, hp, KOBJ_SPOOR, RIGHT_READ, NULL) < 0,
                "outgoing KOBJ_PROCESS refused");
    TEST_ASSERT(handle_get(p, hp, &got) == 0, "the PROCESS slot survives");
    TEST_EXPECT_EQ(got.kind, KOBJ_PROCESS, "outgoing kind unchanged");
    handle_put(&got);

    // Argument validation, all fail-closed.
    TEST_ASSERT(handle_replace(p, -1, KOBJ_SPOOR, RIGHT_READ, NULL) < 0, "negative fd");
    TEST_ASSERT(handle_replace(p, PROC_HANDLE_MAX, KOBJ_SPOOR, RIGHT_READ, NULL) < 0,
                "out-of-range fd");
    TEST_ASSERT(handle_replace(p, h, KOBJ_SPOOR, RIGHT_NONE, NULL) < 0,
                "RIGHT_NONE refused (valid_alloc_args)");
    TEST_ASSERT(handle_replace(p, h, KOBJ_INVALID, RIGHT_READ, NULL) < 0,
                "KOBJ_INVALID refused");

    // An empty slot is not replaceable -- replace is not a back-door alloc.
    hidx_t hfree = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, NULL);
    TEST_ASSERT(hfree >= 0, "third alloc");
    TEST_ASSERT(handle_close(p, hfree) == 0, "close it");
    TEST_ASSERT(handle_replace(p, hfree, KOBJ_SPOOR, RIGHT_READ, NULL) < 0,
                "replacing a CLOSED slot is refused -- replace never creates");

    test_proc_drop(p);
}

// =============================================================================
// #151 close-on-exec. Four tests, one per obligation the flag carries: it is
// ESTABLISHED at install and never inherited; exec CONSUMES exactly the flagged
// slots; fork PRESERVES it; and the dup family sets it per POSIX (clear for
// dup/F_DUPFD, set for F_DUPFD_CLOEXEC).
// =============================================================================

void test_handles_cloexec_lifecycle(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t h = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, p);
    TEST_ASSERT(h >= 0, "alloc failed");

    // Born CLEAR. An fd is not close-on-exec unless something asks.
    TEST_EXPECT_EQ(handle_get_cloexec(p, h), 0, "a fresh handle is not cloexec");

    TEST_ASSERT(handle_set_cloexec(p, h, true) == 0, "set failed");
    TEST_EXPECT_EQ(handle_get_cloexec(p, h), 1, "set then get reads back 1");
    TEST_ASSERT(handle_set_cloexec(p, h, false) == 0, "clear failed");
    TEST_EXPECT_EQ(handle_get_cloexec(p, h), 0, "cleared reads back 0");

    // THE REUSED-INDEX PROPERTY. Flag it, close it, and allocate again: the new
    // occupant lands on the same index (it is the lowest free one) and must NOT
    // inherit a close-on-exec it never asked for. This is the class the tree
    // keeps meeting -- a reused identity serving the previous occupant's state
    // (L1f F1's reused inode, the net-3d slot re-mint).
    //
    // WHAT THIS CANNOT DO, measured rather than assumed: it cannot attribute the
    // property to a SITE. Two places clear the bit (handle_install_locked and
    // handle_close) and the #151 revert probes showed they overlap completely --
    // removing either one alone leaves the whole suite green, and only removing
    // BOTH trips this assertion. So it pins the property and nothing finer, which
    // is the honest reading of a redundancy rather than a defect in the test.
    TEST_ASSERT(handle_set_cloexec(p, h, true) == 0, "set before close");
    TEST_ASSERT(handle_close(p, h) == 0, "close failed");
    hidx_t h2 = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h2 == h, "the reused index is the one just freed");
    TEST_EXPECT_EQ(handle_get_cloexec(p, h2), 0,
                   "a REUSED slot does not inherit the previous fd's cloexec");

    // A free or out-of-range slot has no flag to read or write. Refusing rather
    // than silently succeeding matters: a set on a free index would be a promise
    // about an fd that does not exist, and the next open would inherit it.
    TEST_ASSERT(handle_close(p, h2) == 0, "close h2");
    TEST_ASSERT(handle_get_cloexec(p, h2) < 0, "get on a free slot refuses");
    TEST_ASSERT(handle_set_cloexec(p, h2, true) < 0, "set on a free slot refuses");
    TEST_ASSERT(handle_get_cloexec(p, -1) < 0, "get on a negative index refuses");
    TEST_ASSERT(handle_set_cloexec(p, PROC_HANDLE_MAX, true) < 0,
                "set past the table refuses");

    test_proc_drop(p);
}

void test_handles_cloexec_exec_sweep(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t keep1 = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, p);
    hidx_t go1   = handle_alloc(p, KOBJ_THREAD,  RIGHT_READ, NULL);
    hidx_t keep2 = handle_alloc(p, KOBJ_THREAD,  RIGHT_READ, NULL);
    hidx_t go2   = handle_alloc(p, KOBJ_THREAD,  RIGHT_READ, NULL);
    TEST_ASSERT(keep1 >= 0 && go1 >= 0 && keep2 >= 0 && go2 >= 0, "allocs failed");

    TEST_ASSERT(handle_set_cloexec(p, go1, true) == 0, "flag go1");
    TEST_ASSERT(handle_set_cloexec(p, go2, true) == 0, "flag go2");

    TEST_EXPECT_EQ(handle_table_count(p->handles), 4, "four open before exec");
    TEST_EXPECT_EQ(handle_close_on_exec(p), 2, "exec closes exactly the two flagged");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 2, "two survive");

    // The SURVIVORS are the right ones -- a sweep that closed the complement
    // would report the same count.
    struct Handle got;
    TEST_ASSERT(handle_get(p, keep1, &got) == 0, "keep1 survived");
    handle_put(&got);
    TEST_ASSERT(handle_get(p, keep2, &got) == 0, "keep2 survived");
    handle_put(&got);
    TEST_ASSERT(handle_get(p, go1, &got) < 0, "go1 is gone");
    TEST_ASSERT(handle_get(p, go2, &got) < 0, "go2 is gone");

    // Idempotent: a second sweep finds nothing, because the bits went with the
    // descriptors. A sweep that left them set would close whatever landed on
    // those indices next.
    TEST_EXPECT_EQ(handle_close_on_exec(p), 0, "a second sweep closes nothing");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 2, "and disturbs nothing");

    // A Proc with no flagged fds is untouched -- which is EVERY native Proc,
    // since nothing outside the phenotype can set the flag. That is what makes
    // the exec sweep byte-neutral for native programs.
    hidx_t fresh = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(fresh >= 0, "fresh alloc");
    TEST_EXPECT_EQ(handle_close_on_exec(p), 0, "an unflagged table loses nothing");
    TEST_EXPECT_EQ(handle_table_count(p->handles), 3, "all three still open");

    test_proc_drop(p);
}

void test_handles_cloexec_fork_preserves(void) {
    struct Proc *parent = test_proc_make();
    struct Proc *child  = test_proc_make();
    TEST_ASSERT(parent != NULL && child != NULL, "test_proc_make returned NULL");

    hidx_t plain = handle_alloc(parent, KOBJ_PROCESS, RIGHT_READ, parent);
    hidx_t flagged = handle_alloc(parent, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(plain >= 0 && flagged >= 0, "parent allocs failed");
    TEST_ASSERT(handle_set_cloexec(parent, flagged, true) == 0, "flag it");

    TEST_ASSERT(handle_table_copy_into(child, parent) == 2, "both slots copied");

    // POSIX: fork PRESERVES close-on-exec. A shell sets it once on a bookkeeping
    // fd and then forks per command -- clearing it here would leak that fd into
    // every child, which is exactly what it was set to prevent.
    TEST_EXPECT_EQ(handle_get_cloexec(child, flagged), 1,
                   "the child inherits the flag with the fd");
    TEST_EXPECT_EQ(handle_get_cloexec(child, plain), 0,
                   "and inherits its absence too");

    // The parent is unchanged -- the copy reads, it does not move.
    TEST_EXPECT_EQ(handle_get_cloexec(parent, flagged), 1, "parent keeps its flag");

    // The child's exec then consumes what it inherited, which is the pair of
    // behaviours together: fork preserves, exec consumes.
    TEST_EXPECT_EQ(handle_close_on_exec(child), 1, "the child's exec closes it");
    TEST_EXPECT_EQ(handle_table_count(child->handles), 1, "the plain fd survives");

    test_proc_drop(child);
    test_proc_drop(parent);
}

void test_handles_dup_posix(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t src = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ | RIGHT_TRANSFER, p);
    TEST_ASSERT(src >= 0, "src alloc failed");

    // THE MINIMUM IS THE POINT. A shell's savefd() does F_DUPFD_CLOEXEC(fd, 10)
    // precisely to move its bookkeeping fd out of the low range a user
    // redirection could collide with; returning the first free slot regardless
    // would hand back a low fd and break the guarantee the call was made to get.
    hidx_t d = handle_dup_posix(p, src, 10, /*cloexec=*/true);
    TEST_ASSERT(d >= 10, "F_DUPFD_CLOEXEC(fd, 10) lands at or above 10");
    TEST_EXPECT_EQ(handle_get_cloexec(p, d), 1, "and comes out close-on-exec");

    // Rights come across VERBATIM -- POSIX dup gives the new descriptor the same
    // access as the old.
    struct Handle got;
    TEST_ASSERT(handle_get(p, d, &got) == 0, "get the dup");
    TEST_EXPECT_EQ((u64)got.rights, (u64)(RIGHT_READ | RIGHT_TRANSFER),
                   "the dup carries the parent's rights unchanged");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_PROCESS, "and its kind");
    handle_put(&got);

    // The plain form: lowest free slot, flag CLEAR. POSIX dup(2) explicitly
    // clears close-on-exec on the new descriptor.
    hidx_t d2 = handle_dup_posix(p, src, 0, /*cloexec=*/false);
    TEST_ASSERT(d2 >= 0 && d2 < 10, "F_DUPFD(fd, 0) takes a low free slot");
    TEST_EXPECT_EQ(handle_get_cloexec(p, d2), 0, "dup clears close-on-exec");

    // The rights-REDUCING handle_dup is unchanged by all this, including its
    // flag: it is the capability-surface form and has no POSIX flag to carry.
    TEST_ASSERT(handle_set_cloexec(p, src, true) == 0, "flag the source");
    hidx_t d3 = handle_dup(p, src, RIGHT_READ);
    TEST_ASSERT(d3 >= 0, "handle_dup failed");
    TEST_EXPECT_EQ(handle_get_cloexec(p, d3), 0,
                   "handle_dup's child is not close-on-exec even from a flagged parent");

    // Refusals, fail-closed.
    TEST_ASSERT(handle_dup_posix(p, src, -1, false) < 0, "a negative minimum refuses");
    TEST_ASSERT(handle_dup_posix(p, src, PROC_HANDLE_MAX, false) < 0,
                "a minimum past the table refuses");
    TEST_ASSERT(handle_dup_posix(p, PROC_HANDLE_MAX, 0, false) < 0,
                "an out-of-range source refuses");

    test_proc_drop(p);
}

// #157: dup onto a SPECIFIC index -- what dup2/dup3 needs and what none of the
// three neighbouring primitives does (dup_posix picks the index, replace demands
// a live one, the fork copy demands a free one).
void test_handles_dup_to(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t src = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ | RIGHT_TRANSFER, p);
    TEST_ASSERT(src >= 0, "src alloc failed");

    // (1) INTO A FREE SLOT, at the EXACT index. 40 is far above the lowest free
    // one, so a handle_dup_posix-style "first free >= min" would also land at 40
    // and this leg alone cannot tell them apart -- leg (2) is what does.
    TEST_EXPECT_EQ((int)handle_dup_to(p, src, 40, /*cloexec=*/false), 40,
                   "dup_to returns the index it was given");
    struct Handle got;
    TEST_ASSERT(handle_get(p, 40, &got) == 0, "the target is live");
    TEST_EXPECT_EQ((u64)got.rights, (u64)(RIGHT_READ | RIGHT_TRANSFER),
                   "rights carried VERBATIM (POSIX: same access)");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_PROCESS, "and the kind");
    handle_put(&got);
    TEST_EXPECT_EQ(handle_get_cloexec(p, 40), 0, "cloexec=false was honoured");

    // (2) ONTO A LIVE SLOT -- the leg that separates this from every neighbour.
    // Put a DIFFERENT kind at 41 first so the overwrite is visible in the
    // result rather than only in a refcount.
    hidx_t occ = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(occ >= 0, "occupant alloc failed");
    TEST_ASSERT(handle_dup_to(p, src, occ, /*cloexec=*/false) == occ,
                "dup_to overwrites a LIVE slot rather than refusing it");
    TEST_ASSERT(handle_get(p, occ, &got) == 0, "the overwritten slot is live");
    TEST_EXPECT_EQ((int)got.kind, (int)KOBJ_PROCESS,
                   "it now names the source, not the old occupant");
    handle_put(&got);

    // (3) THE COUNTER ARITHMETIC, which is the only thing that can catch a
    // one-sided bump. Over a LIVE slot one descriptor dies and one is born, so
    // the LIVE count (allocated - freed) must not move; over a FREE slot it
    // must go up by exactly one.
    u64 a0 = handle_total_allocated(), f0 = handle_total_freed();
    TEST_ASSERT(handle_dup_to(p, src, occ, false) == occ, "dup_to over live");
    TEST_EXPECT_EQ((handle_total_allocated() - a0) - (handle_total_freed() - f0),
                   (u64)0, "over a LIVE slot the live count is unchanged");
    u64 a1 = handle_total_allocated(), f1 = handle_total_freed();
    TEST_ASSERT(handle_dup_to(p, src, 42, false) == 42, "dup_to into free");
    TEST_EXPECT_EQ((handle_total_allocated() - a1) - (handle_total_freed() - f1),
                   (u64)1, "into a FREE slot the live count rises by one");

    // (4) close-on-exec is SET FROM THE ARGUMENT -- not inherited from the
    // source (the classic dup2 mistake; POSIX clears it) and not from the slot's
    // previous occupant (the reused-identity failure). Flag BOTH so a wrong
    // answer cannot come out right by accident.
    TEST_ASSERT(handle_set_cloexec(p, src, true) == 0, "flag the source");
    TEST_ASSERT(handle_set_cloexec(p, 40, true) == 0, "flag the occupant");
    TEST_ASSERT(handle_dup_to(p, src, 40, /*cloexec=*/false) == 40, "dup_to");
    TEST_EXPECT_EQ(handle_get_cloexec(p, 40), 0,
                   "cloexec=false wins over BOTH a flagged source and a flagged "
                   "previous occupant");
    TEST_ASSERT(handle_dup_to(p, src, 43, /*cloexec=*/true) == 43, "dup_to");
    TEST_EXPECT_EQ(handle_get_cloexec(p, 43), 1, "and cloexec=true is honoured");

    // (5) REFUSALS, fail-closed. old == new is refused rather than made a no-op:
    // serving it would set cloexec on a descriptor POSIX dup2 returns untouched.
    TEST_ASSERT(handle_dup_to(p, src, src, false) < 0, "old == new refuses");
    TEST_ASSERT(handle_dup_to(p, -1, 44, false) < 0, "a negative source refuses");
    TEST_ASSERT(handle_dup_to(p, src, -1, false) < 0, "a negative target refuses");
    TEST_ASSERT(handle_dup_to(p, PROC_HANDLE_MAX, 44, false) < 0,
                "an out-of-range source refuses");
    TEST_ASSERT(handle_dup_to(p, src, PROC_HANDLE_MAX, false) < 0,
                "an out-of-range target refuses");
    TEST_ASSERT(handle_dup_to(p, 45, 44, false) < 0, "an EMPTY source refuses");

    // Native checked errors are decided by the operation, not a racy re-query.
    TEST_EXPECT_EQ(handle_dup_to_checked(p, src, src, false), -22, "same slot EINVAL");
    TEST_EXPECT_EQ(handle_dup_to_checked(p, 45, 44, false), -9, "absent source EBADF");
    TEST_EXPECT_EQ(handle_dup_to_checked(p, -1, 44, false), -9, "negative EBADF");
    TEST_EXPECT_EQ(handle_dup_to_checked(p, src, PROC_HANDLE_MAX, false), -9, "bounds EBADF");

    // (6) THE ALIAS GATE, the same predicate handle_dup and the fork copy use:
    // a KOBJ_SRV connection Spoor is pinned to its Proc, so a second handle
    // naming it is refused here exactly as it is there. A target left untouched
    // by the refusal is part of the contract.
    hidx_t srv = handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, NULL);
    TEST_ASSERT(srv >= 0, "srv alloc failed");
    TEST_EXPECT_EQ(handle_dup_to_checked(p, srv, 40, true), -13, "forbidden alias EACCES");
    TEST_EXPECT_EQ(handle_get_cloexec(p, 40), 0, "failed alias preserves occupied target flag");
    TEST_ASSERT(handle_dup_to(p, srv, 44, false) < 0,
                "a non-aliasable source refuses (NoSrvDup)");
    TEST_ASSERT(handle_get(p, 44, &got) != 0,
                "and the refusal left the target slot alone");

    test_proc_drop(p);
}

// B-3b: the native lowest-free dup (SYS_HANDLE_DUP_TO + SYS_HANDLE_DUP_LOWEST),
// which Pouch's dup and F_DUPFD stand on. Each refusal is a DIFFERENT errno and
// the guest acts on which one it gets (a shell's redirect probe reads EBADF as
// "not open" and anything else as fatal), so every leg asserts the value.
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);
extern s64 sys_handle_dup_to_for_test(struct Proc *p, u64 old, u64 target, u64 flags);
extern s64 sys_handle_flags_get_for_test(struct Proc *p, u64 h);
extern s64 sys_handle_flags_set_for_test(struct Proc *p, u64 h, u64 flags);

void test_handles_dup_lowest(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t src = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ | RIGHT_TRANSFER, p);
    TEST_ASSERT(src >= 0, "src alloc failed");

    // (1) THE FLOOR SKIPS OCCUPIED SLOTS: 10 and 11 are taken, so the lowest
    // free at or above 10 is 12 -- neither the floor itself (an exact-target
    // dup) nor the table's lowest free slot (a floorless dup).
    TEST_ASSERT(handle_dup_to(p, src, 10, false) == 10, "occupy 10");
    TEST_ASSERT(handle_dup_to(p, src, 11, false) == 11, "occupy 11");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, src, 10, false), 12,
                   "lowest free at or above the floor");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, 12, 12, false), 13,
                   "a source may be its own floor; the result never is");

    // (2) RIGHTS VERBATIM, CLOSE-ON-EXEC FROM THE ARGUMENT.
    TEST_ASSERT(handle_set_cloexec(p, src, true) == 0, "flag the source");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, src, 20, false), 20, "dup at 20");
    TEST_EXPECT_EQ(handle_get_cloexec(p, 20), 0,
                   "a flagged source does not flag the dup");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, src, 21, true), 21, "dup at 21");
    TEST_EXPECT_EQ(handle_get_cloexec(p, 21), 1, "cloexec=true is honoured");
    struct Handle got;
    TEST_ASSERT(handle_get(p, 21, &got) == 0, "get the dup");
    TEST_EXPECT_EQ((u64)got.rights, (u64)(RIGHT_READ | RIGHT_TRANSFER),
                   "the dup carries the source's rights unchanged");
    handle_put(&got);

    // (3) REFUSALS, each with its own errno. A free slot in range is EBADF
    // whatever the floor says: the source is decided first, as Linux's fcntl
    // looks the descriptor up before reading its argument.
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, 45, 0, false), -T_E_BADF,
                   "a free source is EBADF");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, 45, -1, false), -T_E_BADF,
                   "a free source with a bad floor is still EBADF");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, -1, 0, false), -T_E_BADF,
                   "a negative source is EBADF");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, PROC_HANDLE_MAX, 0, false), -T_E_BADF,
                   "a source past the table is EBADF");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, src, -1, false), -T_E_INVAL,
                   "a negative floor is EINVAL");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, src, PROC_HANDLE_MAX, false), -T_E_INVAL,
                   "a floor past the table is EINVAL");
    hidx_t srv = handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, NULL);
    TEST_ASSERT(srv >= 0, "srv alloc failed");
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, srv, 0, false), -T_E_ACCES,
                   "a connection the alias rules pin is EACCES");

    // (4) EMFILE AT THE TOP, with the reference given back. A pipe end is the
    // source because a Spoor's refcount is observable: the refused dup's
    // acquire must be released, or every failed dup leaks one reference.
    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe");
    struct Spoor *rs = (struct Spoor *)p->handles->slots[rd].obj;
    int ref0 = __atomic_load_n(&rs->ref, __ATOMIC_ACQUIRE);
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, rd, PROC_HANDLE_MAX - 1, false),
                   PROC_HANDLE_MAX - 1, "the last slot is free and taken");
    TEST_EXPECT_EQ(__atomic_load_n(&rs->ref, __ATOMIC_ACQUIRE), ref0 + 1,
                   "a successful dup holds one more reference");
    u64 a0 = handle_total_allocated(), f0 = handle_total_freed();
    TEST_EXPECT_EQ(handle_dup_posix_checked(p, rd, PROC_HANDLE_MAX - 1, false),
                   -T_E_MFILE, "nothing free at or above the last slot is EMFILE");
    TEST_EXPECT_EQ(__atomic_load_n(&rs->ref, __ATOMIC_ACQUIRE), ref0 + 1,
                   "the refused dup gave its reference back");
    TEST_EXPECT_EQ((handle_total_allocated() - a0) - (handle_total_freed() - f0),
                   (u64)0, "and installed nothing");

    // (5) THE SYSCALL HANDLER: the flag selects floor semantics, a floor out of
    // range is EINVAL where an exact target out of range is EBADF, and a huge
    // floor cannot narrow into range.
    const u64 low = SYS_HANDLE_DUP_LOWEST;
    const u64 cx  = SYS_HANDLE_CLOSE_ON_EXEC;
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, 10, low), (s64)14,
                   "LOWEST: 10..13 are taken, so 14");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, 30, low | cx), (s64)30,
                   "LOWEST|CLOSE_ON_EXEC at a free floor");
    TEST_EXPECT_EQ(handle_get_cloexec(p, 30), 1, "and the flag is set");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, (u64)PROC_HANDLE_MAX, low),
                   -(s64)T_E_INVAL, "a floor past the table is EINVAL");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, (1ull << 32) + 10, low),
                   -(s64)T_E_INVAL, "a floor that would narrow to 10 is EINVAL");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, 45, (u64)PROC_HANDLE_MAX, low),
                   -(s64)T_E_BADF, "a free source outranks a bad floor");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, (u64)PROC_HANDLE_MAX, 0),
                   -(s64)T_E_BADF, "an EXACT target past the table is EBADF");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, (u64)src, low) > (s64)src, 1,
                   "LOWEST permits source == floor");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, (u64)src, 0),
                   -(s64)T_E_INVAL, "an exact target equal to the source is EINVAL");
    TEST_EXPECT_EQ(sys_handle_dup_to_for_test(p, (u64)src, 31, 4), -(s64)T_E_INVAL,
                   "an unknown flag bit is EINVAL");
    TEST_ASSERT(handle_get(p, 31, &got) != 0, "and the refusal installed nothing");

    test_proc_drop(p);
}

// B-3b: everything F_GETFD and F_GETFL report, in one word read under one lock
// hold. A pipe is the fixture because its two ends are open for different
// access modes and its nonblock flag is real (devpipe acts on it).
void test_handles_status_word(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "test_proc_make returned NULL");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe");
    hidx_t kobj = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ, p);
    TEST_ASSERT(kobj >= 0, "kobj alloc failed");

    // (1) THE PRIMITIVE.
    struct handle_status st;
    TEST_EXPECT_EQ(handle_get_status(p, wr, &st), 0, "status of the write end");
    TEST_EXPECT_EQ((int)st.omode, 1, "the write end is open OWRITE");
    TEST_EXPECT_EQ((int)st.nonblock, 0, "and blocking");
    TEST_EXPECT_EQ((int)st.cloexec, 0, "and not close-on-exec");
    TEST_ASSERT(handle_set_nonblock(p, wr, true) == 0, "set nonblock");
    TEST_ASSERT(handle_set_cloexec(p, wr, true) == 0, "set cloexec");
    TEST_EXPECT_EQ(handle_get_status(p, wr, &st), 0, "status again");
    TEST_EXPECT_EQ((int)st.nonblock, 1, "nonblock reads back");
    TEST_EXPECT_EQ((int)st.cloexec, 1, "cloexec reads back");
    TEST_EXPECT_EQ(handle_get_status(p, kobj, &st), 0, "status of a non-Spoor");
    TEST_EXPECT_EQ((int)st.omode, 0, "a non-Spoor reports no open mode");
    TEST_EXPECT_EQ(handle_get_status(p, 45, &st), -1, "a free slot refuses");
    TEST_EXPECT_EQ(handle_get_status(p, PROC_HANDLE_MAX, &st), -1,
                   "a slot past the table refuses");

    // (2) THE WORD. Each field at its own bit, so a field written to the wrong
    // place reads as a wrong value rather than a lucky one.
    s64 w = sys_handle_flags_get_for_test(p, (u64)wr);
    TEST_EXPECT_EQ(w, (s64)(SYS_HANDLE_CLOSE_ON_EXEC |
                            (1u << SYS_HANDLE_STATUS_OMODE_SHIFT) |
                            SYS_HANDLE_STATUS_NONBLOCK),
                   "write end: cloexec | OWRITE | nonblock");
    TEST_EXPECT_EQ(sys_handle_flags_get_for_test(p, (u64)rd), (s64)0,
                   "read end: nothing set, OREAD is zero");

    // APPEND rides the open mode; no Dev a kernel test can open honours it, so
    // the fixture stamps the read end's open-time mode and restores it. ORDWR
    // in the same stamp proves the access mode is masked, not shifted whole.
    struct Spoor *rs = (struct Spoor *)p->handles->slots[rd].obj;
    int saved = rs->mode;
    rs->mode = 2 | (int)SYS_WALK_OPEN_OAPPEND | (int)SYS_WALK_OPEN_OTRUNC;
    s64 wa = sys_handle_flags_get_for_test(p, (u64)rd);
    rs->mode = saved;
    TEST_EXPECT_EQ(wa, (s64)((2u << SYS_HANDLE_STATUS_OMODE_SHIFT) |
                             SYS_HANDLE_STATUS_APPEND),
                   "ORDWR|OAPPEND|OTRUNC reads as ORDWR + APPEND, OTRUNC dropped");

    TEST_EXPECT_EQ(sys_handle_flags_get_for_test(p, 45), -(s64)T_E_BADF,
                   "a free slot is EBADF");
    TEST_EXPECT_EQ(sys_handle_flags_get_for_test(p, (1ull << 32) + (u64)wr),
                   -(s64)T_E_BADF, "an index that would narrow into range is EBADF");

    // (3) FLAGS_SET takes close-on-exec only: a status bit is not a descriptor
    // flag, and accepting one would promise an F_SETFL this call does not do.
    TEST_EXPECT_EQ(sys_handle_flags_set_for_test(p, (u64)wr, 0), (s64)0, "clear cloexec");
    TEST_EXPECT_EQ(handle_get_cloexec(p, wr), 0, "cleared");
    TEST_EXPECT_EQ(sys_handle_flags_set_for_test(p, (u64)wr, SYS_HANDLE_STATUS_NONBLOCK),
                   -(s64)T_E_INVAL, "a status bit is EINVAL");
    TEST_EXPECT_EQ(sys_handle_flags_set_for_test(p, 45, SYS_HANDLE_CLOSE_ON_EXEC),
                   -(s64)T_E_BADF, "a free slot is EBADF");

    test_proc_drop(p);
}
