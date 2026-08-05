// gic leaf-API smoke. Verifies version detection landed and the
// distributor + redistributor base addresses are non-zero (i.e.,
// gic_init populated state and didn't quietly return without
// configuring anything). Internal GIC bring-up correctness is tested
// implicitly by the timer.tick_increments test that follows — if the
// distributor / redist / CPU interface init were broken, no timer
// IRQs would fire.

#include "test.h"

#include "../../arch/arm64/gic.h"
#include "../../arch/arm64/hwfeat.h"
#include <thylacine/dtb.h>
#include <thylacine/smp.h>
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

// V-4c-3 F5 (task #73): the per-CPU IRQ counter's slots must not share a cache
// coherency granule -- every CPU stores to its own slot on every interrupt, so
// two slots in one granule is a write-contended line on the kernel's hottest
// path. The separation is bought with compile-time padding (CACHE_LINE_MAX_BYTES
// in hwfeat.h), which means only a RUNTIME check can hold that constant to
// account against the hardware's actual granule. That is what this test is for:
// a target whose CWG outgrows the pad fails here, loudly, instead of quietly
// resuming the false sharing the padding was added to stop.
void test_gic_cpu_irq_counter_geometry(void) {
    // Alignment, not just size. Padding each slot to a granule separates the
    // slots from each other; only aligning the array keeps slot 0 out of the
    // granule occupied by whatever BSS precedes it.
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        const void *slot = gic_cpu_irq_count_slot_addr(i);
        TEST_ASSERT(slot != 0, "every in-range CPU has an IRQ counter slot");
        TEST_ASSERT(((uintptr_t)slot % CACHE_LINE_MAX_BYTES) == 0,
                    "IRQ counter slot is coherency-granule aligned");
    }

    // Adjacent slots are a whole granule apart -- the property that actually
    // ends the sharing. Checking the stride rather than sizeof() means a future
    // edit that pads the struct but drops the array's alignment, or vice versa,
    // still fails.
    if (DTB_MAX_CPUS >= 2) {
        uintptr_t a = (uintptr_t)gic_cpu_irq_count_slot_addr(0);
        uintptr_t b = (uintptr_t)gic_cpu_irq_count_slot_addr(1);
        TEST_ASSERT(b - a == CACHE_LINE_MAX_BYTES,
                    "adjacent IRQ counter slots are one full granule apart");
    }
    TEST_ASSERT(gic_cpu_irq_count_slot_addr(DTB_MAX_CPUS) == 0,
                "out-of-range CPU has no slot");

    // The pad is only sufficient if it is at least the hardware's writeback
    // granule. CWG == 0 means the part declines to report one (QEMU TCG does),
    // which is an absence of information rather than a small granule -- so it
    // cannot fail this check, and the pad stands on the architectural bound
    // instead. A part that DOES report is authoritative and must fit.
    // Measured on both accels at the time of writing: CWG == 64 under HVF
    // `-cpu host` (the real M2) and under TCG `-cpu max` alike, so the pad
    // carries 2x margin and the cwg == 0 branch below is currently DEAD on
    // both -- kept because the architecture permits it, not because it runs.
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        const struct hw_cpu_ident *id = hw_cpu_ident(i);
        if (!id) continue;   // PSCI bring-up failure: no identity recorded
        if (id->cwg == 0) continue;
        TEST_ASSERT((id->cwg & (id->cwg - 1)) == 0,
                    "CTR_EL0.CWG decodes to a power of two");
        TEST_ASSERT(id->cwg <= CACHE_LINE_MAX_BYTES,
                    "CACHE_LINE_MAX_BYTES covers this part's writeback granule "
                    "(bump it -- the IRQ counter padding is now too small)");
    }
}
