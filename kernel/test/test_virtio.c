// VirtIO core tests (P4-F).
//
// Covers DTB probe + MMIO register reads + virtqueue allocation.
// Tests against the QEMU virt 32 virtio-mmio slots; -device flags in
// tools/run-vm.sh / tools/test.sh attach concrete devices (virtio-rng
// at v1.0 P4-F).

#include "test.h"

#include <thylacine/handle.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>

void test_virtio_mmio_probe(void);
void test_virtio_magic_value(void);
void test_virtio_version_modern(void);
void test_virtio_rng_present(void);
void test_virtio_negotiate_features_smoke(void);
void test_virtio_virtqueue_alloc_destroy(void);
void test_virtio_find_by_device_id(void);
void test_virtio_reset_in_range_no_match(void);
void test_virtio_vq_size_for(void);
void test_virtio_proc_death_quiesces_device(void);

// =============================================================================
// Tests.
// =============================================================================

void test_virtio_mmio_probe(void) {
    int n = virtio_mmio_dev_count();
    // QEMU virt provides 32 virtio-mmio slots in DTB. Some bare-board
    // configs may have fewer; require at least 1 so the probe path
    // is exercised.
    TEST_ASSERT(n >= 1, "virtio_init must discover at least 1 MMIO slot");
    TEST_ASSERT(n <= 32, "virtio_init must not over-discover (cap=32)");
}

void test_virtio_magic_value(void) {
    if (virtio_mmio_dev_count() < 1) return;

    struct virtio_mmio_dev *d = virtio_mmio_dev_get(0);
    TEST_ASSERT(d != NULL, "first slot present");
    u32 magic = virtio_mmio_read32(d, VIRTIO_MMIO_MAGIC_VALUE);
    TEST_EXPECT_EQ(magic, (u32)VIRTIO_MMIO_MAGIC,
                   "first slot MagicValue == 'virt'");
}

void test_virtio_version_modern(void) {
    if (virtio_mmio_dev_count() < 1) return;

    // Version field per-slot. QEMU virt's slots may report version 1
    // (legacy) or version 2 (modern) depending on QEMU build defaults
    // and whether a device is attached. Both are spec-valid; the
    // virtqueue_alloc test verifies the modern transport's
    // QueueDesc/Driver/Device-low/high split actually works against
    // whatever the device reports. Here we only assert version is in
    // the valid spec range [1, 2].
    for (int i = 0; i < virtio_mmio_dev_count(); i++) {
        struct virtio_mmio_dev *d = virtio_mmio_dev_get(i);
        TEST_ASSERT(d->version == VIRTIO_MMIO_VERSION_LEGACY ||
                    d->version == VIRTIO_MMIO_VERSION_MODERN,
                    "every probed slot reports version 1 or 2");
    }
}

void test_virtio_rng_present(void) {
    // Tests run with `-device virtio-rng-device,id=rng0` in the QEMU
    // command line (per tools/run-vm.sh). Look for a slot with
    // DeviceID = 4 (RNG). If absent, skip — environments without the
    // -device flag (a hypothetical bare-board boot) keep the rest of
    // the test suite valid.
    struct virtio_mmio_dev *rng =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_RNG);
    if (!rng) return;

    TEST_EXPECT_EQ(rng->device_id, (u32)VIRTIO_DEVICE_ID_RNG,
                   "rng device id matches");
    TEST_ASSERT(rng->base != NULL, "rng MMIO mapped");
}

void test_virtio_negotiate_features_smoke(void) {
    struct virtio_mmio_dev *rng =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_RNG);
    if (!rng) return;

    // virtio-rng has no required features at v1.0 — passing 0 selects
    // an empty driver feature set; the device acks FEATURES_OK.
    bool ok = virtio_negotiate_features(rng, 0);
    TEST_ASSERT(ok, "feature negotiation succeeds with empty driver mask");

    // After successful negotiation, the device's STATUS should still
    // have FEATURES_OK set (we re-check inside negotiate, but verify
    // the state is observable from outside).
    u32 status = virtio_get_status(rng);
    TEST_ASSERT((status & VIRTIO_STATUS_ACKNOWLEDGE) != 0,
                "ACKNOWLEDGE set");
    TEST_ASSERT((status & VIRTIO_STATUS_DRIVER) != 0,
                "DRIVER set");
    TEST_ASSERT((status & VIRTIO_STATUS_FEATURES_OK) != 0,
                "FEATURES_OK set");

    // Reset back so a future test starts from a clean slate.
    virtio_reset(rng);
}

void test_virtio_virtqueue_alloc_destroy(void) {
    struct virtio_mmio_dev *rng =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_RNG);
    if (!rng) return;

    // Renegotiate features fresh — virtqueue_create requires the
    // device be in a state where FEATURES_OK has been written. Per
    // VIRTIO 1.2 §3.1.1 step 6 happens after step 5.
    TEST_ASSERT(virtio_negotiate_features(rng, 0),
                "feature negotiation prerequisite");

    struct virtio_virtqueue *vq = virtio_virtqueue_create(rng, 0);
    TEST_ASSERT(vq != NULL, "virtio_virtqueue_create(rng, queue 0) OK");

    TEST_ASSERT(vq->size > 0 && vq->size <= 64,
                "queue size in (0, 64]");
    TEST_ASSERT(vq->desc != NULL, "desc ring KVA non-NULL");
    TEST_ASSERT(vq->avail != NULL, "avail ring KVA non-NULL");
    TEST_ASSERT(vq->used != NULL, "used ring KVA non-NULL");

    // Page-aligned PAs.
    TEST_EXPECT_EQ((u64)vq->desc_pa & 0xFFFu, (u64)0,
                   "desc_pa page-aligned");
    TEST_EXPECT_EQ((u64)vq->avail_pa & 0xFFFu, (u64)0,
                   "avail_pa page-aligned");
    TEST_EXPECT_EQ((u64)vq->used_pa & 0xFFFu, (u64)0,
                   "used_pa page-aligned");

    // Desc / avail / used live in independent pages — modern transport
    // requires this. Our impl always allocates 3 separate pages so the
    // PAs cannot collide.
    TEST_ASSERT(vq->desc_pa != vq->avail_pa, "desc != avail PA");
    TEST_ASSERT(vq->desc_pa != vq->used_pa,  "desc != used PA");
    TEST_ASSERT(vq->avail_pa != vq->used_pa, "avail != used PA");

    virtio_virtqueue_destroy(vq);

    // Reset back to clean state for downstream tests.
    virtio_reset(rng);
}

void test_virtio_find_by_device_id(void) {
    // Lookup of an unattached device-id returns NULL.
    struct virtio_mmio_dev *bogus =
        virtio_mmio_find_by_device_id(0xDEADBEEFu);
    TEST_ASSERT(bogus == NULL, "lookup of unattached device-id returns NULL");

    // virtio_mmio_dev_get out-of-range returns NULL.
    TEST_ASSERT(virtio_mmio_dev_get(-1) == NULL, "negative idx NULL");
    TEST_ASSERT(virtio_mmio_dev_get(10000) == NULL, "huge idx NULL");
}

// =============================================================================
// RW-7 R3-F1 / R3-F6: device-death quiesce + queue-size negotiation.
// =============================================================================

void test_virtio_reset_in_range_no_match(void) {
    // A PA range containing no probed slot resets nothing. Also exercises
    // the overflow + zero-size guards (return 0 before the loop -- no OOB).
    TEST_EXPECT_EQ(virtio_mmio_reset_in_range(0xF000000000ull, PAGE_SIZE), 0,
                   "out-of-range PA resets no devices");
    TEST_EXPECT_EQ(virtio_mmio_reset_in_range(0xFFFFFFFFFFFFF000ull, 0x4000), 0,
                   "pa+size overflow resets nothing (guard, no OOB)");
    TEST_EXPECT_EQ(virtio_mmio_reset_in_range(0x1000, 0), 0,
                   "zero size resets nothing");
}

void test_virtio_vq_size_for(void) {
    // Clamp to the single-page default.
    TEST_EXPECT_EQ(virtio_vq_size_for(256), (u32)VIRTIO_VQ_NUM_DEFAULT,
                   "QueueNumMax >= default clamps to default");
    TEST_EXPECT_EQ(virtio_vq_size_for(VIRTIO_VQ_NUM_DEFAULT),
                   (u32)VIRTIO_VQ_NUM_DEFAULT, "exact default kept");
    TEST_EXPECT_EQ(virtio_vq_size_for(32), (u32)32, "pow2 below default kept");
    TEST_EXPECT_EQ(virtio_vq_size_for(1), (u32)1, "1 is a valid pow2 size");
    // R3-F6 reject: zero + non-power-of-two -> 0 (unsupported queue).
    TEST_EXPECT_EQ(virtio_vq_size_for(0), (u32)0, "QueueNumMax 0 unsupported");
    TEST_EXPECT_EQ(virtio_vq_size_for(48), (u32)0, "non-pow2 (48) rejected");
    TEST_EXPECT_EQ(virtio_vq_size_for(3), (u32)0, "non-pow2 (3) rejected");
}

// Find a probed virtio-mmio page whose every slot is empty (device_id==0) --
// safe to reset in a test without disturbing the live virtio-rng (no driver,
// no random.c interaction behind an empty slot). Sets *page + *count; returns
// false when no all-empty page exists (the test then skips).
static bool find_empty_virtio_page(u64 *page_out, int *count_out) {
    int n = virtio_mmio_dev_count();
    for (int i = 0; i < n; i++) {
        u64 cand = (u64)virtio_mmio_dev_get(i)->pa & ~((u64)PAGE_SIZE - 1);
        int slots = 0;
        bool all_empty = true;
        for (int j = 0; j < n; j++) {
            struct virtio_mmio_dev *d = virtio_mmio_dev_get(j);
            u64 dpa = (u64)d->pa;
            if (dpa >= cand && dpa < cand + PAGE_SIZE) {
                slots++;
                if (d->device_id != 0) all_empty = false;
            }
        }
        if (all_empty && slots >= 1) {
            *page_out  = cand;
            *count_out = slots;
            return true;
        }
    }
    return false;
}

void test_virtio_proc_death_quiesces_device(void) {
    // R3-F1 regression: a Proc holding a KObj_MMIO over a virtio page resets
    // every device in that range when its devices are quiesced at exit/reap
    // -- the kernel backstop against a crashed driver DMAing into freed
    // pages. Pre-fix, proc_quiesce_owned_devices did not exist and the
    // device stayed armed across the KObj_DMA free.
    u64 page = 0;
    int expected = 0;
    if (!find_empty_virtio_page(&page, &expected)) return;  // skip: no empty page

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct KObj_MMIO *km = kobj_mmio_create(page, PAGE_SIZE);
    TEST_ASSERT(km != NULL, "kobj_mmio_create over an empty virtio page");

    hidx_t h = handle_alloc(p, KOBJ_MMIO,
                            RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, km);
    TEST_ASSERT(h >= 0, "handle_alloc(KOBJ_MMIO) consumes the create ref");

    // The crux: walk(handles) -> match(PA range) -> virtio_reset, counted.
    int n = proc_quiesce_owned_devices(p);
    TEST_EXPECT_EQ(n, expected,
                   "proc death resets every virtio slot in the held range");

    // proc_free re-runs the quiesce (harmless re-reset) then handle_table_free
    // -> kobj_mmio_unref releases the claim. Mirrors drop_test_proc.
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}
