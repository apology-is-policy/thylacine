// VirtIO core tests (P4-F).
//
// Covers DTB probe + MMIO register reads + virtqueue allocation.
// Tests against the QEMU virt 32 virtio-mmio slots; -device flags in
// tools/run-vm.sh / tools/test.sh attach concrete devices (virtio-rng
// at v1.0 P4-F).

#include "test.h"

#include <thylacine/types.h>
#include <thylacine/virtio.h>

void test_virtio_mmio_probe(void);
void test_virtio_magic_value(void);
void test_virtio_version_modern(void);
void test_virtio_rng_present(void);
void test_virtio_negotiate_features_smoke(void);
void test_virtio_virtqueue_alloc_destroy(void);
void test_virtio_find_by_device_id(void);

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
