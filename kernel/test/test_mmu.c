// I-12's predicate, pte_violates_wxe: the check every leaf encoder in mmu.c
// runs before a table can hold the value (the encoders themselves are pinned
// by the wxe_install_* fault-test variants, which need the extinction).
//
// The rows are written out, not computed: deriving the expected answer from
// the same "writable && (PXN clear || UXN clear)" the predicate uses would
// pass a predicate that is wrong in the same way.

#include "test.h"

#include "../../arch/arm64/mmu.h"

#include <thylacine/types.h>

struct wxe_row {
    u64  ap;
    bool pxn, uxn;
    bool violates;
};

static const struct wxe_row g_wxe_rows[] = {
    // EL1-writable, no EL0 access: executable at EL1 (PXN clear) or at EL0
    // (UXN clear -- EL0 instruction fetch ignores AP) is W+X.
    { PTE_AP_RW_EL1, false, false, true  },
    { PTE_AP_RW_EL1, false, true,  true  },
    { PTE_AP_RW_EL1, true,  false, true  },
    { PTE_AP_RW_EL1, true,  true,  false },
    // Writable at both ELs: the same three violate.
    { PTE_AP_RW_ANY, false, false, true  },
    { PTE_AP_RW_ANY, false, true,  true  },
    { PTE_AP_RW_ANY, true,  false, true  },
    { PTE_AP_RW_ANY, true,  true,  false },
    // Read-only at EL1 (EL0 none, or EL0 read-only): never writable.
    { PTE_AP_RO_EL1, false, false, false },
    { PTE_AP_RO_EL1, false, true,  false },
    { PTE_AP_RO_EL1, true,  false, false },
    { PTE_AP_RO_EL1, true,  true,  false },
    { PTE_AP_RO_ANY, false, false, false },
    { PTE_AP_RO_ANY, false, true,  false },
    { PTE_AP_RO_ANY, true,  false, false },
    { PTE_AP_RO_ANY, true,  true,  false },
};

void test_mmu_wxe_predicate(void) {
    for (size_t i = 0; i < sizeof(g_wxe_rows) / sizeof(g_wxe_rows[0]); i++) {
        const struct wxe_row *r = &g_wxe_rows[i];
        u64 pte = PTE_VALID | PTE_TYPE_PAGE | PTE_AF | r->ap |
                  (r->pxn ? PTE_PXN : 0) | (r->uxn ? PTE_UXN : 0);
        TEST_EXPECT_EQ(pte_violates_wxe(pte), r->violates,
                       "pte_violates_wxe disagrees with its AP x PXN x UXN row");
        // An invalid descriptor maps nothing, whatever its other bits say.
        TEST_EXPECT_EQ(pte_violates_wxe(pte & ~PTE_VALID), false,
                       "pte_violates_wxe flagged an invalid descriptor");
    }
    // The attributes the kernel encodes with are clean; each one, with its
    // execute-never bit for an EL cleared, is not (text is not writable, so
    // only the writable ones turn).
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_TEXT), false, "PTE_KERN_TEXT flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_RO), false, "PTE_KERN_RO flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_RW), false, "PTE_KERN_RW flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_RW_BLOCK), false, "PTE_KERN_RW_BLOCK flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_RO_BLOCK), false, "PTE_KERN_RO_BLOCK flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_DEVICE_RW), false, "PTE_DEVICE_RW flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_DEVICE_RW_BLOCK), false, "PTE_DEVICE_RW_BLOCK flagged");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_RW & ~PTE_PXN), true,
                   "an EL1-executable PTE_KERN_RW passed");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_RW & ~PTE_UXN), true,
                   "an EL0-executable PTE_KERN_RW passed");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_DEVICE_RW & ~PTE_UXN), true,
                   "an EL0-executable PTE_DEVICE_RW passed");
    TEST_EXPECT_EQ(pte_violates_wxe(PTE_KERN_TEXT & ~PTE_UXN), false,
                   "read-only text executable at EL0 was flagged as writable");
}
