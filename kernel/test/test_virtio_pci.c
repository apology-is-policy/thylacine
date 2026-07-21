// Tests for kernel/virtio_pci.c (P4-H).
//
// Exercises ECAM-based enumeration of VirtIO-PCI devices on bus 0 of
// the QEMU virt PCIe root complex. tools/run-vm.sh attaches at least
// one VirtIO PCI device (virtio-rng-pci) so the count assertions hold
// in the integration boot.

#include "test.h"

#include <thylacine/virtio.h>
#include <thylacine/virtio_pci.h>

void test_virtio_pci_init_called(void);
void test_virtio_pci_count_within_bound(void);
void test_virtio_pci_devices_have_vendor(void);
void test_virtio_pci_devices_have_cfg(void);
void test_virtio_pci_find_rng(void);
void test_virtio_pci_find_unknown_returns_null(void);
void test_virtio_pci_cfg_read_bounds(void);
void test_virtio_pci_cfg_write_bounds(void);

// virtio_pci_init has already run (in boot_main before tests). The
// device count reflects whatever QEMU virt + tools/run-vm.sh's
// -device flags exposed. tools/run-vm.sh attaches virtio-rng-pci, so
// count >= 1 in this configuration.
void test_virtio_pci_init_called(void) {
    int count = virtio_pci_dev_count();
    TEST_ASSERT(count >= 0, "virtio_pci_dev_count returned negative");
    TEST_ASSERT(count <= (int)VIRTIO_PCI_MAX_DEVS,
                "virtio_pci_dev_count exceeds VIRTIO_PCI_MAX_DEVS");
}

void test_virtio_pci_count_within_bound(void) {
    int count = virtio_pci_dev_count();
    TEST_ASSERT(count >= 1,
                "expected at least 1 VirtIO PCI device "
                "(virtio-rng-pci via tools/run-vm.sh)");
}

void test_virtio_pci_devices_have_vendor(void) {
    int count = virtio_pci_dev_count();
    for (int i = 0; i < count; i++) {
        struct virtio_pci_dev *d = virtio_pci_dev_get(i);
        TEST_ASSERT(d != NULL, "dev_get returned NULL for in-range idx");
        TEST_EXPECT_EQ(d->vendor_id, VIRTIO_PCI_VENDOR_ID,
                       "non-VirtIO vendor in g_virtio_pci_devs[]");
        // Device IDs must be in one of the two VirtIO ranges.
        bool legacy = d->device_id >= VIRTIO_PCI_DEVICE_ID_LEGACY_MIN
                   && d->device_id <= VIRTIO_PCI_DEVICE_ID_LEGACY_MAX;
        bool modern = d->device_id >= VIRTIO_PCI_DEVICE_ID_MODERN_MIN
                   && d->device_id <= VIRTIO_PCI_DEVICE_ID_MODERN_MAX;
        TEST_ASSERT(legacy || modern,
                    "device_id outside VirtIO PCI ranges");
        TEST_EXPECT_EQ((u64)d->is_modern, (u64)modern,
                       "is_modern flag inconsistent with device_id range");
    }
}

void test_virtio_pci_devices_have_cfg(void) {
    int count = virtio_pci_dev_count();
    for (int i = 0; i < count; i++) {
        struct virtio_pci_dev *d = virtio_pci_dev_get(i);
        TEST_ASSERT(d->cfg != NULL, "d->cfg should be a mapped KVA");
        // Read vendor_id back through the public accessor — round-trip.
        u16 v = virtio_pci_cfg_read16(d, PCI_CFG_VENDOR_ID);
        TEST_EXPECT_EQ((u64)v, (u64)d->vendor_id,
                       "cfg_read16 vendor_id differs from cached");
    }
}

// tools/run-vm.sh attaches virtio-rng-pci, which exposes
// virtio_device_id = VIRTIO_DEVICE_ID_RNG (4) on modern transport.
// QEMU may also synthesize other PCI VirtIO devices on the default
// machine; this test asserts the specific RNG is among them.
void test_virtio_pci_find_rng(void) {
    struct virtio_pci_dev *d =
        virtio_pci_find_by_device_id(VIRTIO_DEVICE_ID_RNG, 0);
    TEST_ASSERT(d != NULL,
                "virtio_pci_find_by_device_id(VIRTIO_DEVICE_ID_RNG) returned NULL");
    TEST_EXPECT_EQ((u64)d->virtio_device_id, (u64)VIRTIO_DEVICE_ID_RNG,
                   "find_by_device_id returned mismatched device");
}

void test_virtio_pci_find_unknown_returns_null(void) {
    // 0xFFFE is not a defined VIRTIO device_id at v1.0.
    struct virtio_pci_dev *d = virtio_pci_find_by_device_id(0xFFFE, 0);
    TEST_ASSERT(d == NULL,
                "virtio_pci_find_by_device_id(0xFFFE) should be NULL");
}

void test_virtio_pci_cfg_read_bounds(void) {
    struct virtio_pci_dev *d = virtio_pci_dev_get(0);
    if (!d) {
        // Empty enum (no PCI devices) — not our concern here.
        return;
    }
    // Out-of-range offset: 4096 is exactly 1 past the function's cfg space.
    u32 ov = virtio_pci_cfg_read32(d, 4096);
    TEST_EXPECT_EQ((u64)ov, (u64)0xFFFFFFFFu,
                   "out-of-range cfg_read32 should return 0xFFFFFFFF");
    // Misaligned access.
    u16 ov16 = virtio_pci_cfg_read16(d, 1);
    TEST_EXPECT_EQ((u64)ov16, (u64)0xFFFFu,
                   "misaligned cfg_read16 should return 0xFFFF");
    u32 ov32 = virtio_pci_cfg_read32(d, 2);
    TEST_EXPECT_EQ((u64)ov32, (u64)0xFFFFFFFFu,
                   "misaligned cfg_read32 should return 0xFFFFFFFF");
    // NULL d: defensive.
    u8 ov8 = virtio_pci_cfg_read8(NULL, 0);
    TEST_EXPECT_EQ((u64)ov8, (u64)0xFFu,
                   "NULL d cfg_read8 should return 0xFF");
}

// pci-1a: the config-space WRITE helpers honor the same bounds as the
// reads -- an out-of-range, misaligned, or NULL-device write is a
// no-op. Proven by observing the (read-only) vendor ID is unchanged
// after a battery of bad-bounds writes. The positive round-trip is
// covered by pci-1b's BAR-sizing test (write 0xFFFFFFFF, read the size
// mask), the standard PCI protocol QEMU implements.
void test_virtio_pci_cfg_write_bounds(void) {
    struct virtio_pci_dev *d = virtio_pci_dev_get(0);
    if (!d) {
        return;     // no PCI device — covered by count_within_bound
    }
    u16 vendor_before = virtio_pci_cfg_read16(d, PCI_CFG_VENDOR_ID);
    TEST_EXPECT_EQ((u64)vendor_before, (u64)VIRTIO_PCI_VENDOR_ID,
                   "virtio-pci vendor should be 0x1AF4 before writes");

    virtio_pci_cfg_write32(d, 4096, 0xDEADBEEFu);   // out of range -> no-op
    virtio_pci_cfg_write32(d, 2, 0xDEADBEEFu);      // misaligned   -> no-op
    virtio_pci_cfg_write16(d, 1, 0xBEEFu);          // misaligned   -> no-op
    virtio_pci_cfg_write8(NULL, 0, 0xFFu);          // NULL device  -> no-op

    u16 vendor_after = virtio_pci_cfg_read16(d, PCI_CFG_VENDOR_ID);
    TEST_EXPECT_EQ((u64)vendor_after, (u64)vendor_before,
                   "bad-bounds cfg writes must not alter vendor ID");
}
