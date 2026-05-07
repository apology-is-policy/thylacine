// BURROW refcount + mapping lifecycle tests (P2-Fd / P3-Db).
//
// Six original tests cover the dual-refcount lifecycle. Two more added at
// P3-Db cover the new burrow_map(Proc, ...) / burrow_unmap(Proc, ...) entry
// points; see test_vmo_map_proc.c.
//
//   burrow.create_close_round_trip
//     burrow_create_anon → burrow_unref. Verify cumulative counters increment.
//
//   burrow.refcount_lifecycle
//     create + ref + unref + unref → freed at last unref. Verifies that
//     handle_count tracking is accurate; pages stay alive through the
//     intermediate count of 1.
//
//   burrow.map_unmap_lifecycle
//     create + map + unref + unmap → freed only on unmap. Verifies that
//     handle_count alone reaching 0 does NOT free if mapping_count > 0.
//     Uses the bare refcount ops (burrow_acquire_mapping /
//     burrow_release_mapping) directly so the lifecycle is exercised in
//     isolation without going through the VMA layer.
//
//   burrow.handles_x_mappings_matrix
//     Cross-product: close-handle-then-unmap, unmap-then-close, multiple
//     handles + multiple mappings. Each combination should free exactly
//     once at the right boundary.
//
//   burrow.via_handle_table
//     End-to-end through the handle table: burrow_create_anon + handle_alloc
//     (KOBJ_BURROW) + handle_dup + handle_close (both) → unrefs propagate;
//     BURROW freed on the last close.
//
//   burrow.handle_table_orphan_cleanup
//     handle_table_free with an open KOBJ_BURROW handle releases the BURROW
//     reference (the orphan-cleanup path used by proc_free).
//
// Maps to specs/burrow.tla:
//   - create_close_round_trip + refcount_lifecycle: HandleOpen +
//     HandleClose action sequences; NoUseAfterFree invariant.
//   - map_unmap_lifecycle: MapVmo + UnmapVmo; NoUseAfterFree.
//   - handles_x_mappings_matrix: cross-product of all spec actions
//     verifying the iff-form NoUseAfterFree invariant.
//   - via_handle_table: integration with handles.tla's HandleAlloc /
//     HandleClose / HandleDup actions for KOBJ_BURROW.

#include "test.h"

#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/burrow.h>

void test_vmo_create_close_round_trip(void);
void test_vmo_refcount_lifecycle(void);
void test_vmo_map_unmap_lifecycle(void);
void test_vmo_handles_x_mappings_matrix(void);
void test_vmo_via_handle_table(void);
void test_vmo_handle_table_orphan_cleanup(void);
void test_vmo_size_overflow_rejected(void);
void test_vmo_dup_oom_rollback(void);

// Convenience: test "did we free a BURROW this turn" by snapshot diff.
static u64 created_diff_snap;
static u64 destroyed_diff_snap;

static void snap_counters(void) {
    created_diff_snap   = burrow_total_created();
    destroyed_diff_snap = burrow_total_destroyed();
}

static u64 created_since_snap(void) {
    return burrow_total_created() - created_diff_snap;
}

static u64 destroyed_since_snap(void) {
    return burrow_total_destroyed() - destroyed_diff_snap;
}

void test_vmo_create_close_round_trip(void) {
    snap_counters();

    struct Burrow *v = burrow_create_anon(4096);
    TEST_ASSERT(v != NULL, "burrow_create_anon(4096) returned NULL");
    TEST_EXPECT_EQ(burrow_get_size(v), (size_t)4096, "size mismatch");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1,
        "fresh BURROW must have handle_count=1");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 0,
        "fresh BURROW must have mapping_count=0");
    TEST_EXPECT_EQ(created_since_snap(), (u64)1, "1 create");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)0, "0 destroyed");

    // unref → both counts 0 → freed.
    burrow_unref(v);
    // v is now an invalid pointer — must NOT dereference.
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)1,
        "BURROW must be destroyed by the unref that brings handle_count to 0 "
        "(when mapping_count was already 0)");
}

void test_vmo_refcount_lifecycle(void) {
    snap_counters();

    struct Burrow *v = burrow_create_anon(8192);   // 2 pages
    TEST_ASSERT(v != NULL, "burrow_create_anon(8192) NULL");
    TEST_EXPECT_EQ(burrow_get_size(v), (size_t)8192, "size 8192");

    // Acquire a second handle. handle_count = 2.
    burrow_ref(v);
    TEST_EXPECT_EQ(burrow_handle_count(v), 2, "ref → handle_count=2");

    // Release one. handle_count = 1; not yet freed.
    burrow_unref(v);
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "unref → handle_count=1");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)0,
        "must not be freed while handle_count > 0");

    // Release the last. handle_count = 0; mapping_count = 0; freed.
    burrow_unref(v);
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)1,
        "freed at last unref");
}

void test_vmo_map_unmap_lifecycle(void) {
    snap_counters();

    struct Burrow *v = burrow_create_anon(4096);
    TEST_ASSERT(v != NULL, "burrow_create_anon NULL");

    // Open a mapping. mapping_count = 1, handle_count = 1.
    burrow_acquire_mapping(v);
    TEST_EXPECT_EQ(burrow_mapping_count(v), 1, "map → mapping_count=1");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "handle_count unchanged by map");

    // Release the handle. handle_count = 0; mapping_count = 1; pages
    // STILL alive (mapping holds them). Spec's NoUseAfterFree:
    // mapping_count > 0 ⇒ pages alive.
    burrow_unref(v);
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)0,
        "must not be freed while mapping_count > 0 even with handle_count=0");
    TEST_EXPECT_EQ(burrow_handle_count(v), 0, "handle_count = 0");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 1, "mapping_count = 1");

    // Release the last mapping. Now both counts = 0; freed.
    burrow_release_mapping(v);
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)1,
        "freed at last unmap when handle_count was already 0");
}

void test_vmo_handles_x_mappings_matrix(void) {
    // Six combinations to exhaust the dual-refcount free boundary.
    // Each subtest asserts the BURROW is freed at exactly the right moment
    // (no premature, no delayed).
    snap_counters();

    // 1. close-handle-then-unmap (asymmetric: handle 1 + map 1)
    {
        struct Burrow *v = burrow_create_anon(4096);
        TEST_ASSERT(v, "create_1 NULL");
        burrow_acquire_mapping(v);
        u64 before_destroys = destroyed_since_snap();
        burrow_unref(v);
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 1: handle close while mapped — must NOT free");
        burrow_release_mapping(v);
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys + 1,
            "case 1: unmap brings both to 0 — MUST free");
    }

    // 2. unmap-then-close-handle (mirror of 1)
    {
        struct Burrow *v = burrow_create_anon(4096);
        TEST_ASSERT(v, "create_2 NULL");
        burrow_acquire_mapping(v);
        u64 before_destroys = destroyed_since_snap();
        burrow_release_mapping(v);
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 2: unmap while handle held — must NOT free");
        burrow_unref(v);
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys + 1,
            "case 2: handle close brings both to 0 — MUST free");
    }

    // 3. multiple handles + one mapping; close handles first
    {
        struct Burrow *v = burrow_create_anon(4096);
        TEST_ASSERT(v, "create_3 NULL");
        burrow_ref(v);                  // handle_count = 2
        burrow_acquire_mapping(v);                  // mapping_count = 1
        u64 before_destroys = destroyed_since_snap();
        burrow_unref(v);                // handle_count = 1
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 3a: handle 1→0 with handle_count still > 0 — no free");
        burrow_unref(v);                // handle_count = 0
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 3b: last handle close while mapping_count > 0 — no free");
        burrow_release_mapping(v);                // mapping_count = 0
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys + 1,
            "case 3c: last unmap — free");
    }

    // 4. multiple mappings + one handle; unmap first
    {
        struct Burrow *v = burrow_create_anon(4096);
        TEST_ASSERT(v, "create_4 NULL");
        burrow_acquire_mapping(v); burrow_acquire_mapping(v);      // mapping_count = 2
        u64 before_destroys = destroyed_since_snap();
        burrow_release_mapping(v);                // mapping_count = 1
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 4a: unmap with mapping_count still > 0 — no free");
        burrow_release_mapping(v);                // mapping_count = 0
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 4b: last unmap with handle_count > 0 — no free");
        burrow_unref(v);                // handle_count = 0
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys + 1,
            "case 4c: last handle close — free");
    }

    // 5. interleaved: ref, map, unref, ref, unmap, unref, unref
    {
        struct Burrow *v = burrow_create_anon(4096);
        TEST_ASSERT(v, "create_5 NULL");
        burrow_ref(v);                  // h=2
        burrow_acquire_mapping(v);                  // m=1
        burrow_unref(v);                // h=1
        burrow_ref(v);                  // h=2
        burrow_release_mapping(v);                // m=0
        u64 before_destroys = destroyed_since_snap();
        burrow_unref(v);                // h=1
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 5: still has h=1 — no free");
        burrow_unref(v);                // h=0, m=0 — free
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys + 1,
            "case 5: last unref — free");
    }

    // 6. only handles, no mappings (handle-only lifecycle)
    {
        struct Burrow *v = burrow_create_anon(4096);
        TEST_ASSERT(v, "create_6 NULL");
        burrow_ref(v); burrow_ref(v); burrow_ref(v);   // h=4
        u64 before_destroys = destroyed_since_snap();
        burrow_unref(v); burrow_unref(v); burrow_unref(v);
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys,
            "case 6a: 3 of 4 handles closed — no free");
        burrow_unref(v);                // last handle
        TEST_EXPECT_EQ(destroyed_since_snap(), before_destroys + 1,
            "case 6b: last handle close — free");
    }
}

// Helper: allocate a fresh test Proc, run a body, drop. Mirrors the
// pattern in test_handle.c.
static struct Proc *test_proc_make_for_vmo(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    return p;
}

static void test_proc_drop_for_vmo(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_vmo_via_handle_table(void) {
    snap_counters();
    u64 created_before = created_diff_snap;
    u64 destroyed_before = destroyed_diff_snap;

    struct Proc *p = test_proc_make_for_vmo();
    TEST_ASSERT(p, "test_proc_make NULL");

    struct Burrow *v = burrow_create_anon(4096);
    TEST_ASSERT(v, "burrow_create_anon NULL");

    // handle_alloc on a KOBJ_BURROW. The BURROW's handle_count was set to 1
    // by burrow_create_anon; handle_alloc does NOT increment (the count
    // already accounts for this initial handle).
    hidx_t h = handle_alloc(p, KOBJ_BURROW, RIGHT_READ | RIGHT_MAP, v);
    TEST_ASSERT(h >= 0, "handle_alloc(KOBJ_BURROW) failed");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1,
        "BURROW handle_count remains 1 after handle_alloc (count was already 1 from create)");

    // handle_dup increments via burrow_ref. Now handle_count = 2.
    hidx_t hd = handle_dup(p, h, RIGHT_READ);
    TEST_ASSERT(hd >= 0, "handle_dup failed");
    TEST_EXPECT_EQ(burrow_handle_count(v), 2,
        "handle_dup must burrow_ref the underlying BURROW (handle_count = 2)");

    // Close the dup. handle_count = 1; not freed.
    TEST_EXPECT_EQ(handle_close(p, hd), 0, "close dup");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1,
        "handle_close on dup must burrow_unref (handle_count = 1)");
    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, (u64)0,
        "must not be freed while handle_count > 0");

    // Close the original. handle_count = 0; mapping_count = 0; FREE.
    TEST_EXPECT_EQ(handle_close(p, h), 0, "close original");
    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, (u64)1,
        "BURROW must be freed on the last handle close");
    TEST_EXPECT_EQ(burrow_total_created() - created_before, (u64)1,
        "exactly 1 BURROW created");

    test_proc_drop_for_vmo(p);
}

void test_vmo_size_overflow_rejected(void) {
    // P2-Fd self-audit (pre-R5-F): burrow_create_anon's `(size + PAGE_SIZE
    // - 1) / PAGE_SIZE` arithmetic wraps when size is within
    // (PAGE_SIZE - 1) of SIZE_MAX, producing page_count = 0 and a
    // 1-page BURROW claiming size = 0. The fix in burrow_create_anon adds an
    // explicit overflow guard before the arithmetic; this test pins the
    // rejection.
    u64 destroyed_before = burrow_total_destroyed();

    // Just within the wrap boundary — must reject.
    struct Burrow *v1 = burrow_create_anon((size_t)-1);    // SIZE_MAX
    TEST_EXPECT_EQ(v1, NULL,
        "burrow_create_anon(SIZE_MAX) must return NULL (overflow guard)");

    struct Burrow *v2 = burrow_create_anon((size_t)-2);    // SIZE_MAX - 1
    TEST_EXPECT_EQ(v2, NULL,
        "burrow_create_anon(SIZE_MAX-1) must return NULL (overflow guard)");

    // No allocations performed; counter unchanged.
    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, (u64)0,
        "rejected requests must not allocate or destroy any BURROW");

    // Sanity: a normal small size still works.
    struct Burrow *v3 = burrow_create_anon(4096);
    TEST_ASSERT(v3 != NULL,
        "burrow_create_anon(4096) must succeed (overflow guard not over-broad)");
    burrow_unref(v3);
}

void test_vmo_dup_oom_rollback(void) {
    // R5-F F55 close: handle_dup acquires (burrow_ref) BEFORE alloc;
    // on alloc failure (table full), it releases (burrow_unref) for
    // rollback. The acquire/release dance must net to zero —
    // forgetting either side would leak a ref or underflow the count.
    //
    // Construction: fill the handle table with KOBJ_BURROW duplicates
    // of a single BURROW; verify handle_count tracks the slot count;
    // attempt one more dup (must fail); verify handle_count is
    // UNCHANGED (rollback worked); cleanup closes all handles and
    // verifies the BURROW is freed exactly once.
    u64 destroyed_before = burrow_total_destroyed();

    struct Proc *p = test_proc_make_for_vmo();
    TEST_ASSERT(p, "test_proc_make NULL");

    struct Burrow *v = burrow_create_anon(4096);
    TEST_ASSERT(v, "burrow_create_anon NULL");
    // After burrow_create_anon: handle_count = 1 (consumed reference).

    hidx_t parent = handle_alloc(p, KOBJ_BURROW, RIGHT_READ, v);
    TEST_ASSERT(parent >= 0, "parent alloc failed");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1,
        "after handle_alloc(KOBJ_BURROW) — count remains 1 (consumed-ref convention)");

    // Dup until the table is full. The first dup goes into a fresh
    // slot; subsequent dups fill the remaining slots. Each successful
    // dup increments handle_count via burrow_ref.
    int successes = 0;
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        hidx_t hd = handle_dup(p, parent, RIGHT_READ);
        if (hd < 0) break;
        successes++;
    }
    // PROC_HANDLE_MAX slots total; 1 used by parent; PROC_HANDLE_MAX-1
    // successful dups before the table is full.
    TEST_EXPECT_EQ(successes, PROC_HANDLE_MAX - 1,
        "should fit PROC_HANDLE_MAX-1 dups alongside the parent");
    TEST_EXPECT_EQ(handle_table_count(p->handles), PROC_HANDLE_MAX,
        "table should be full");
    TEST_EXPECT_EQ(burrow_handle_count(v), PROC_HANDLE_MAX,
        "handle_count should equal slot count (parent + dups)");

    // The next dup MUST fail (table full). The acquire happened
    // (burrow_ref → count + 1); the alloc failed; the release fires
    // (burrow_unref → count - 1). Net: count unchanged.
    hidx_t failed = handle_dup(p, parent, RIGHT_READ);
    TEST_EXPECT_EQ(failed, -1,
        "dup past PROC_HANDLE_MAX must return -1");
    TEST_EXPECT_EQ(burrow_handle_count(v), PROC_HANDLE_MAX,
        "handle_dup's acquire/release rollback must net to zero "
        "(count unchanged after failed dup)");
    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, (u64)0,
        "rollback must NOT trigger a free");

    // Cleanup. test_proc_drop calls proc_free which calls
    // handle_table_free which closes every slot via handle_release_obj
    // → burrow_unref. After all 64 closes, count = 0 + mapping_count = 0
    // → BURROW freed exactly once.
    test_proc_drop_for_vmo(p);
    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, (u64)1,
        "BURROW must be freed exactly once after proc_free closes all handles");
}

void test_vmo_handle_table_orphan_cleanup(void) {
    // Verifies the orphan-cleanup path in handle_table_free correctly
    // releases BURROW references — proc_free relies on this for cleanup
    // when a Proc dies with open handles (the normal lifecycle, since
    // userspace processes don't always close everything before exit).
    u64 destroyed_before = burrow_total_destroyed();

    struct Proc *p = test_proc_make_for_vmo();
    TEST_ASSERT(p, "test_proc_make NULL");

    struct Burrow *v = burrow_create_anon(4096);
    TEST_ASSERT(v, "burrow_create_anon NULL");

    hidx_t h = handle_alloc(p, KOBJ_BURROW, RIGHT_READ, v);
    TEST_ASSERT(h >= 0, "handle_alloc(KOBJ_BURROW) failed");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "handle_count=1 via the table");

    // proc_free → handle_table_free → handle_release_obj → burrow_unref.
    // BURROW's handle_count drops to 0 (mapping_count is 0); freed.
    test_proc_drop_for_vmo(p);

    TEST_EXPECT_EQ(burrow_total_destroyed() - destroyed_before, (u64)1,
        "BURROW must be freed when proc_free orphan-cleans the handle table");
    // Note: cannot assert burrow_handle_count(v) — v is now invalid.
}
