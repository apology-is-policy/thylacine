// dtb.c leaf-API tests — DTB parser surface coverage.
//
// We can't trivially construct synthetic DTB blobs in the kernel
// environment without a host-side build (no malloc, no host runtime).
// Instead we exercise the parser against the LIVE boot DTB that
// `phys_init`'s caller already validated, and verify the chosen-seed
// readers return non-zero — which they MUST, since KASLR's banner
// shows a successfully-derived offset.
//
// This is a regression check, not a black-box parser test. If the
// parser silently breaks (e.g., a future refactor mishandles the
// chosen-walk), this test fires immediately.

#include "test.h"

#include <thylacine/dtb.h>
#include <thylacine/types.h>

void test_dtb_chosen_kaslr_seed_present(void) {
    TEST_ASSERT(dtb_is_ready(),
        "DTB parser should be initialized post-phys_init");

    u64 kaslr_seed = dtb_get_chosen_kaslr_seed();
    u64 rng_seed   = dtb_get_chosen_rng_seed();

    // QEMU virt populates BOTH /chosen/kaslr-seed (newer QEMU) AND
    // /chosen/rng-seed (always). At least one of them must be
    // non-zero, otherwise our entropy chain fell back to cntpct.
    TEST_ASSERT(kaslr_seed != 0 || rng_seed != 0,
        "DTB /chosen must publish at least one seed");

    // Total size sanity: a real DTB is at least 200 bytes (header is
    // 40 bytes; the structure block + strings round it up). 4 GiB is
    // an obvious upper bound.
    u32 totalsize = dtb_get_total_size();
    TEST_ASSERT(totalsize >= 0xC8 && totalsize < 0xFFFFFFFFu,
        "DTB total_size should be reasonable");
}
