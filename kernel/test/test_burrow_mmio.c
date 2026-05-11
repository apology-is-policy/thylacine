// P4-Ic1: burrow_create_mmio lifecycle tests.
//
// Per <thylacine/burrow.h> + specs/burrow.tla. A MMIO Burrow holds a
// reference on its underlying KObj_MMIO so the PA-exclusivity claim
// survives the userspace handle's close. burrow_unref of an MMIO type
// skips free_pages (no alloc_pages backing) and releases the held
// kobj_mmio reference; the underlying KObj_MMIO is only freed when
// ALL holders (userspace handle + this Burrow ref + any future
// Phase 5+ holders) have unref'd.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_burrow_mmio_create_basic(void);
void test_burrow_mmio_create_null_rejected(void);
void test_burrow_mmio_create_holds_kobj_ref(void);
void test_burrow_mmio_unref_releases_kobj_ref(void);
void test_burrow_mmio_acquire_mapping_works(void);
void test_burrow_mmio_lifecycle_round_trip(void);

// Synthetic PA range — chosen to NOT overlap any real MMIO region.
// kobj_mmio_create only tracks the claim; it doesn't access memory.
#define TEST_BURROW_PA   0x100050000ull
#define TEST_BURROW_SIZE 0x2000ull       // 2 pages

void test_burrow_mmio_create_basic(void) {
    struct KObj_MMIO *km = kobj_mmio_create(TEST_BURROW_PA, TEST_BURROW_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");

    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");
    TEST_EXPECT_EQ((int)b->type, (int)BURROW_TYPE_MMIO, "wrong type");
    TEST_EXPECT_EQ(b->size, (size_t)TEST_BURROW_SIZE, "wrong size");
    TEST_EXPECT_EQ(b->pa, (u64)TEST_BURROW_PA, "wrong pa");
    TEST_ASSERT(b->pages == NULL, "MMIO Burrow should have pages=NULL");
    TEST_EXPECT_EQ(b->handle_count, 1, "construction ref should be 1");
    TEST_EXPECT_EQ(b->mapping_count, 0, "mapping_count starts at 0");
    TEST_ASSERT(b->kobj_mmio == km, "kobj_mmio field not set correctly");

    burrow_unref(b);    // frees b + drops the Burrow's kobj_mmio ref
    kobj_mmio_unref(km);  // drops caller's ref → kobj freed + claim released
}

// NULL kobj_mmio rejected; magic-mismatch panics (UAF defense).
void test_burrow_mmio_create_null_rejected(void) {
    struct Burrow *b = burrow_create_mmio(NULL);
    TEST_ASSERT(b == NULL, "burrow_create_mmio(NULL) should return NULL");
    // The corrupted-magic case is an extinction-on-misuse path; we
    // don't exercise it here (would tear down the test harness).
}

// burrow_create_mmio takes a ref on the underlying KObj_MMIO. Observable
// by checking that the kobj's live count stays at 1 across the
// burrow_create + burrow_unref pair (the Burrow's ref is added then
// released; live count is unchanged on net).
void test_burrow_mmio_create_holds_kobj_ref(void) {
    u64 live_before = kobj_mmio_live_count();

    struct KObj_MMIO *km = kobj_mmio_create(TEST_BURROW_PA, TEST_BURROW_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1,
                   "kobj_mmio_create didn't bump live");

    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");
    // Live count unchanged — Burrow's ref doesn't create a new KObj.
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1,
                   "Burrow create shouldn't bump kobj live count");

    // Drop caller's ref on km. Live count stays bumped because Burrow
    // still holds a ref.
    kobj_mmio_unref(km);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1,
                   "kobj released too early — Burrow ref not held");

    // Drop Burrow. Now both refs gone → kobj freed → live count drops.
    burrow_unref(b);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before,
                   "burrow_unref didn't release kobj_mmio ref");
}

// Symmetric: caller-side unref happens first, then Burrow unref. Same
// final state.
void test_burrow_mmio_unref_releases_kobj_ref(void) {
    u64 live_before = kobj_mmio_live_count();

    struct KObj_MMIO *km = kobj_mmio_create(TEST_BURROW_PA, TEST_BURROW_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");
    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");

    // Drop Burrow first. kobj still alive because caller holds km ref.
    burrow_unref(b);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1,
                   "kobj should stay alive (caller still holds ref)");

    // Drop caller's ref. Now kobj is freed.
    kobj_mmio_unref(km);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before,
                   "kobj should be freed after both refs dropped");
}

// burrow_acquire_mapping works for MMIO Burrows (the dual-count
// mechanics are type-independent).
void test_burrow_mmio_acquire_mapping_works(void) {
    struct KObj_MMIO *km = kobj_mmio_create(TEST_BURROW_PA, TEST_BURROW_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");
    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");

    burrow_acquire_mapping(b);
    TEST_EXPECT_EQ(b->mapping_count, 1, "mapping_count not bumped");

    // Drop construction ref. handle_count → 0 but mapping_count=1 so
    // Burrow stays alive (NoUseAfterFree).
    burrow_unref(b);
    // Can't dereference b->mapping_count safely if it was freed, but
    // we know it wasn't because mapping_count was 1. Test the
    // counter via burrow_mapping_count which has the magic check.
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "burrow freed prematurely?");

    burrow_release_mapping(b);    // mapping_count → 0; both 0 → free
    kobj_mmio_unref(km);           // drop caller's km ref
}

// Full lifecycle smoke: create kobj → create burrow → acquire mapping →
// release mapping → unref burrow → unref kobj. Verify total destroyed
// counters at the end.
void test_burrow_mmio_lifecycle_round_trip(void) {
    u64 destroyed_before = burrow_total_destroyed();
    u64 live_kobj_before = kobj_mmio_live_count();

    struct KObj_MMIO *km = kobj_mmio_create(TEST_BURROW_PA, TEST_BURROW_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");
    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");

    burrow_acquire_mapping(b);
    burrow_release_mapping(b);    // mapping_count → 0; handle_count still 1
    burrow_unref(b);              // handle_count → 0; both 0 → freed
    kobj_mmio_unref(km);          // drop caller's ref

    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
                   "burrow destroyed counter didn't bump");
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_kobj_before,
                   "kobj live count didn't return to baseline");
}
