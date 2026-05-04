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
    TEST_EXPECT_EQ((u64)gic_version(), (u64)GIC_VERSION_V3,
                   "gic_version != V3 (autodetect failed?)");
    TEST_EXPECT_NE(gic_dist_base(), 0ULL,
                   "gic_dist_base is zero (gic_init didn't run?)");
    TEST_EXPECT_NE(gic_redist_base(), 0ULL,
                   "gic_redist_base is zero (gic_init didn't run?)");
}
