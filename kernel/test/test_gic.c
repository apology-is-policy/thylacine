// gic leaf-API smoke. Verifies version detection landed and the
// distributor + redistributor base addresses are non-zero (i.e.,
// gic_init populated state and didn't quietly return without
// configuring anything). Internal GIC bring-up correctness is tested
// implicitly by the timer.tick_increments test that follows — if the
// distributor / redist / CPU interface init were broken, no timer
// IRQs would fire.

#include "test.h"

#include "../../arch/arm64/gic.h"
#include <thylacine/types.h>

void test_gic_init_smoke(void) {
    // Autodetect landed as one of the two supported versions (DTB-driven;
    // v3 under run-vm.sh's default gic-version=3, v2 under THYLACINE_GIC=2).
    gic_version_t v = gic_version();
    TEST_ASSERT(v == GIC_VERSION_V3 || v == GIC_VERSION_V2,
                "gic_version is neither V2 nor V3 (autodetect failed?)");
    TEST_EXPECT_NE(gic_dist_base(), 0ULL,
                   "gic_dist_base is zero (gic_init didn't run?)");
    // The CPU-side region differs by version: v3 has a redistributor, v2 has
    // the GICC MMIO interface. Whichever the running GIC is, its base must be
    // populated (and the other left zero).
    if (v == GIC_VERSION_V3) {
        TEST_EXPECT_NE(gic_redist_base(), 0ULL,
                       "gic_redist_base is zero on v3 (gic_init didn't run?)");
    } else {
        TEST_EXPECT_NE(gic_cpu_iface_base(), 0ULL,
                       "gic_cpu_iface_base is zero on v2 (gic_init didn't run?)");
    }
}
