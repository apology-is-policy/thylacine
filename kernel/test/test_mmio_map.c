// P4-Ic2: kernel-side tests for MMIO user-VA mapping.
//
// At v1.0 P4-Ic2 we don't exercise the SYS_MMIO_MAP syscall path from
// userspace (that requires a userspace driver — lands at P4-Ic5+ via
// usr/virtio-blk/). Instead we drive `burrow_create_mmio` + `burrow_map`
// directly from kernel context (kproc) to verify:
//
//   1. The Burrow-MMIO + VMA install pipeline works end-to-end.
//   2. burrow_map's mapping_count handoff matches the anon pattern
//      (construction ref → mapping ref via vma_alloc → unref to transfer
//      ownership to the VMA).
//   3. After proc_free, the VMA + Burrow + KObj_MMIO refs all drop
//      coherently (no leaks).
//
// The actual PTE-install + device-memory-PTE-attrs path is exercised
// transitively at P4-Ic5+ when a userspace driver maps + reads MMIO.
// For this chunk, code review of the userland_demand_page changes +
// `make_user_pte_l3(device_memory=true)` path is the verification.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../../arch/arm64/uart.h"

void test_mmio_map_install_vma(void);
void test_mmio_map_overlap_rejected(void);
void test_mmio_map_proc_free_releases_kobj(void);

// Synthetic MMIO range; chosen to not overlap any real device.
#define MMIO_MAP_PA      0x100060000ull
#define MMIO_MAP_SIZE    0x1000ull
#define USER_VA_MMIO     0x40000000ull        // 1 GiB into user-VA

// Forward decls for proc helpers (defined in proc.c — exposed via test).
extern struct Proc *proc_alloc(void);
extern void         proc_free(struct Proc *p);

// burrow_create_mmio + burrow_map installs a VMA reachable via
// vma_lookup. Verifies the integration pipeline at the kernel API
// level.
void test_mmio_map_install_vma(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_MMIO *km = kobj_mmio_create(MMIO_MAP_PA, MMIO_MAP_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");

    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");

    int rc = burrow_map(p, b, USER_VA_MMIO, MMIO_MAP_SIZE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map failed");

    // Drop construction ref — VMA's mapping_count keeps the Burrow alive.
    burrow_unref(b);

    // VMA should be reachable at USER_VA_MMIO.
    struct Vma *vma = vma_lookup(p, USER_VA_MMIO);
    TEST_ASSERT(vma != NULL, "vma_lookup didn't find the new VMA");
    TEST_EXPECT_EQ(vma->vaddr_start, (u64)USER_VA_MMIO, "wrong vaddr_start");
    TEST_EXPECT_EQ(vma->vaddr_end,   (u64)(USER_VA_MMIO + MMIO_MAP_SIZE),
                   "wrong vaddr_end");
    TEST_ASSERT(vma->burrow != NULL, "VMA's burrow is NULL");
    TEST_EXPECT_EQ((int)vma->burrow->type, (int)BURROW_TYPE_MMIO,
                   "VMA's burrow has wrong type");
    TEST_EXPECT_EQ(vma->burrow->pa, (u64)MMIO_MAP_PA, "VMA's burrow has wrong pa");

    // Clean up — proc_free walks VMAs + Burrows, releases everything.
    p->state = 2;     // PROC_STATE_ZOMBIE — required for proc_free
    proc_free(p);

    // After proc_free, the Burrow's mapping_count went to 0, triggering
    // burrow_free_internal which dropped the kobj_mmio ref. The caller's
    // km handle is still alive (we held a ref).
    kobj_mmio_unref(km);
}

// Two map calls to overlapping user-VA ranges: second must be rejected.
void test_mmio_map_overlap_rejected(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_MMIO *km1 = kobj_mmio_create(MMIO_MAP_PA, MMIO_MAP_SIZE);
    TEST_ASSERT(km1 != NULL, "km1 create failed");
    struct Burrow *b1 = burrow_create_mmio(km1);
    TEST_ASSERT(b1 != NULL, "b1 create failed");
    int rc = burrow_map(p, b1, USER_VA_MMIO, MMIO_MAP_SIZE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "first map should succeed");
    burrow_unref(b1);

    // Try to map a different MMIO range at the SAME user-VA.
    struct KObj_MMIO *km2 = kobj_mmio_create(MMIO_MAP_PA + MMIO_MAP_SIZE * 2,
                                             MMIO_MAP_SIZE);
    TEST_ASSERT(km2 != NULL, "km2 create failed");
    struct Burrow *b2 = burrow_create_mmio(km2);
    TEST_ASSERT(b2 != NULL, "b2 create failed");
    rc = burrow_map(p, b2, USER_VA_MMIO, MMIO_MAP_SIZE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, -1, "overlapping map should be rejected");
    burrow_unref(b2);    // claim released since map failed

    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    kobj_mmio_unref(km1);
    kobj_mmio_unref(km2);
}

// proc_free path correctly tears down: VMA → burrow_release_mapping →
// burrow_free_internal → kobj_mmio_unref → claim released. Verified via
// kobj live counter returning to baseline.
void test_mmio_map_proc_free_releases_kobj(void) {
    u64 live_before = kobj_mmio_live_count();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_MMIO *km = kobj_mmio_create(MMIO_MAP_PA, MMIO_MAP_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create failed");
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1, "kobj live +1");

    struct Burrow *b = burrow_create_mmio(km);
    TEST_ASSERT(b != NULL, "burrow_create_mmio failed");
    int rc = burrow_map(p, b, USER_VA_MMIO, MMIO_MAP_SIZE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map failed");
    burrow_unref(b);   // transfer ref to VMA

    // Drop caller's km ref BEFORE proc_free. The VMA's Burrow still
    // holds one ref → kobj still alive.
    kobj_mmio_unref(km);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before + 1,
                   "kobj should stay alive (VMA's Burrow holds ref)");

    // proc_free walks VMAs → burrow_release_mapping → kobj_mmio_unref.
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    TEST_EXPECT_EQ(kobj_mmio_live_count(), live_before,
                   "kobj should be freed after proc_free");
}
