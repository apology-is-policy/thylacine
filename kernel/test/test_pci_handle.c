// Tests for kernel/pci_handle.c (pci-1b) — the KObj_PCI claim lifecycle.
//
// Exercises kobj_pci_claim against the live, idle virtio-rng-pci function
// (tools/run-vm.sh attaches virtio-rng-pci; the kernel CSPRNG drives the
// SEPARATE virtio-rng-MMIO slot, so rng-pci is undriven and safe to claim,
// program BARs on, and quiesce). No virtqueue / DRIVER_OK handshake is done, so
// the enabled bus-master never DMAs; the claim is unref'd promptly, leaving the
// device decode-off.
//
// Tests capture state + unref BEFORE asserting (TEST_ASSERT returns on failure),
// so a mid-test failure never strands a live claim into the next test (which
// would then fail the exclusivity check spuriously).

#include "test.h"

#include <thylacine/mmio_handle.h>     // kobj_mmio_pa_claimed
#include <thylacine/page.h>            // PAGE_SIZE
#include <thylacine/pci_handle.h>
#include <thylacine/virtio.h>          // VIRTIO_DEVICE_ID_RNG
#include <thylacine/virtio_pci.h>      // virtio_pci_find_by_device_id
#include <thylacine/handle.h>          // handle_get/put/close, RIGHT_*, KOBJ_PCI
#include <thylacine/sched.h>           // current_thread
#include <thylacine/thread.h>          // struct Thread / Proc
#include <thylacine/vma.h>             // VMA_PROT_*

void test_pci_bar_decode_size(void);
void test_pci_claim_rng(void);
void test_pci_claim_unknown(void);
void test_pci_claim_exclusive(void);
void test_pci_unref_releases_bars(void);
void test_pci_live_count_balances(void);
void test_pci_syscall_reject(void);
void test_pci_syscall_claim_info(void);

// The pci-1c syscall handlers (non-static, like sys_clock_gettime_handler) --
// driven directly to exercise the cap gate / handle mint / validation paths.
// The SUCCESS copy-out (SYS_PCI_INFO) + the BAR burrow-map (SYS_PCI_MAP_BAR)
// need a real user buffer + user address space, which the in-kernel kproc
// context cannot supply, so those are proven by the pci-2 userspace probe.
extern s64 sys_pci_claim_handler(u64 virtio_device_id, u64 a1);
extern s64 sys_pci_map_bar_handler(u64 hraw, u64 vaddr, u64 bar_index, u64 prot_raw);
extern s64 sys_pci_info_handler(u64 hraw, u64 info_va);

// Deterministic (no-hardware) regression for the BAR-size decode. A 32-bit
// BAR's size mask occupies only the low 32 bits, so it MUST be inverted in
// 32-bit width -- a 64-bit invert sets the upper 32 bits and yields a bogus
// multi-exabyte size (the pci-1b self-found bug, where lo_mask=0xfffff000
// decoded to 0xffffffff00001000 instead of 0x1000 and failed BAR allocation).
void test_pci_bar_decode_size(void) {
    // 32-bit BARs: hi=0, is64=false. The exact bug vector first.
    TEST_EXPECT_EQ(pci_bar_decode_size(0xfffff000u, 0u, false), 0x1000ull,
                   "32-bit 4 KiB BAR must decode to 0x1000 (not a 64-bit invert)");
    TEST_EXPECT_EQ(pci_bar_decode_size(0xffffff00u, 0u, false), 0x100ull,
                   "32-bit 256 B BAR mis-decoded");
    TEST_EXPECT_EQ(pci_bar_decode_size(0xfff00000u, 0u, false), 0x100000ull,
                   "32-bit 1 MiB BAR mis-decoded");
    TEST_EXPECT_EQ(pci_bar_decode_size(0x80000000u, 0u, false), 0x80000000ull,
                   "32-bit 2 GiB BAR mis-decoded");

    // 64-bit BARs: invert the full (hi:lo) combined mask.
    TEST_EXPECT_EQ(pci_bar_decode_size(0xffffc000u, 0xffffffffu, true), 0x4000ull,
                   "64-bit 16 KiB BAR mis-decoded");
    TEST_EXPECT_EQ(pci_bar_decode_size(0xf0000000u, 0xffffffffu, true), 0x10000000ull,
                   "64-bit 256 MiB BAR mis-decoded");
    TEST_EXPECT_EQ(pci_bar_decode_size(0x00000000u, 0xfffffffeu, true),
                   0x200000000ull,
                   "64-bit 8 GiB BAR (high-dword mask only) mis-decoded");

    // Degenerate: a full-width writable mask (2^N) is rejected as 0.
    TEST_EXPECT_EQ(pci_bar_decode_size(0x00000000u, 0u, false), 0x0ull,
                   "unimplemented 32-bit mask must decode to 0");
    TEST_EXPECT_EQ(pci_bar_decode_size(0x00000000u, 0u, true), 0x0ull,
                   "full-width 64-bit mask (2^64) must decode to 0");
}

// A device-id that is never present on the v1.0 VirtIO device set.
#define PCI_TEST_ABSENT_DEVICE_ID  0xFFFEu

// True if the idle virtio-rng-pci is present (the claim target). Tests skip
// gracefully on a config without it (the cfg_read_bounds test sets the
// precedent); tools/run-vm.sh always attaches it, so the claim path runs in the
// integration boot.
static bool rng_pci_present(void) {
    return virtio_pci_find_by_device_id(VIRTIO_DEVICE_ID_RNG) != NULL;
}

// Claim the idle rng-pci, validate the assigned BARs + the resolved COMMON_CFG
// region + the swizzled INTID, then release it.
void test_pci_claim_rng(void) {
    if (!rng_pci_present()) return;

    struct KObj_PCI *k = kobj_pci_claim(VIRTIO_DEVICE_ID_RNG);

    // Capture everything we assert on, then unref, so the device is released
    // regardless of any assertion outcome.
    bool got            = (k != NULL);
    bool magic_ok       = false;
    int  present_bars   = 0;
    bool bars_ok        = true;        // every present BAR is well-formed
    bool common_present = false;
    u8   common_bar     = 0xFF;
    bool intid_valid    = false;
    u32  intid          = 0;

    if (k) {
        magic_ok = (k->magic == KOBJ_PCI_MAGIC);
        for (u32 i = 0; i < PCI_BAR_COUNT; i++) {
            if (!k->bars[i].present) continue;
            present_bars++;
            if (k->bars[i].pa == 0)                      bars_ok = false;
            if ((k->bars[i].pa & (PAGE_SIZE - 1)) != 0)  bars_ok = false;
            if (k->bars[i].size == 0)                    bars_ok = false;
            if (k->bars[i].mmio == NULL)                 bars_ok = false;
        }
        struct pci_region *cr = &k->regions[VIRTIO_PCI_CAP_COMMON_CFG - 1];
        common_present = cr->present;
        common_bar     = cr->bar;
        intid_valid    = k->intid_valid;
        intid          = k->intid;

        kobj_pci_unref(k);
    }

    TEST_ASSERT(got, "kobj_pci_claim(RNG) returned NULL for a present device");
    TEST_ASSERT(magic_ok, "claimed KObj_PCI has a bad magic");
    TEST_ASSERT(present_bars >= 1, "rng-pci claim assigned no memory BAR");
    TEST_ASSERT(bars_ok, "an assigned BAR is malformed (pa/size/align/mmio)");
    // A modern virtio-pci device exposes a common-config region; rng-pci is
    // modern (device_id 0x1044).
    TEST_ASSERT(common_present, "COMMON_CFG region did not resolve from the cap walk");
    TEST_ASSERT(common_bar < PCI_BAR_COUNT, "COMMON_CFG region references an out-of-range BAR");
    // The DTB interrupt-map is present on QEMU virt (proven by dtb.pci_intx_route).
    TEST_ASSERT(intid_valid, "INTx routing did not resolve from the DTB");
    TEST_ASSERT(intid >= 35 && intid <= 38, "swizzled INTID outside the [35,38] window");
}

// Claiming a device-id with no matching VirtIO-PCI function returns NULL.
void test_pci_claim_unknown(void) {
    struct KObj_PCI *k = kobj_pci_claim(PCI_TEST_ABSENT_DEVICE_ID);
    if (k) kobj_pci_unref(k);            // defensive — should never happen
    TEST_ASSERT(k == NULL, "claim of an absent device-id should return NULL");
}

// A second claim of an already-claimed function is rejected; a re-claim after
// the first is released succeeds (the exclusivity slot is freed on unref).
void test_pci_claim_exclusive(void) {
    if (!rng_pci_present()) return;

    struct KObj_PCI *k1 = kobj_pci_claim(VIRTIO_DEVICE_ID_RNG);
    struct KObj_PCI *k2 = kobj_pci_claim(VIRTIO_DEVICE_ID_RNG);   // must fail while k1 alive

    bool k1_ok      = (k1 != NULL);
    bool k2_blocked = (k2 == NULL);

    if (k2) kobj_pci_unref(k2);          // defensive cleanup on the bug path
    if (k1) kobj_pci_unref(k1);

    // After releasing k1, the function is re-claimable.
    struct KObj_PCI *k3 = kobj_pci_claim(VIRTIO_DEVICE_ID_RNG);
    bool k3_ok = (k3 != NULL);
    if (k3) kobj_pci_unref(k3);

    TEST_ASSERT(k1_ok, "first rng-pci claim failed");
    TEST_ASSERT(k2_blocked, "second claim of a live function should be rejected (exclusivity)");
    TEST_ASSERT(k3_ok, "re-claim after release failed (exclusivity slot not freed)");
}

// The last unref releases every assigned BAR's PA claim back to g_mmio_claims.
void test_pci_unref_releases_bars(void) {
    if (!rng_pci_present()) return;

    struct KObj_PCI *k = kobj_pci_claim(VIRTIO_DEVICE_ID_RNG);
    if (!k) { TEST_ASSERT(false, "rng-pci claim failed"); return; }

    u64  pa[PCI_BAR_COUNT];
    bool before[PCI_BAR_COUNT];
    int  n = 0;
    for (u32 i = 0; i < PCI_BAR_COUNT; i++) {
        if (!k->bars[i].present) continue;
        pa[n]     = k->bars[i].pa;
        before[n] = kobj_mmio_pa_claimed(pa[n], PAGE_SIZE);   // claimed while alive
        n++;
    }

    kobj_pci_unref(k);   // no user mapping took a burrow ref -> the claim frees

    bool after[PCI_BAR_COUNT];
    for (int j = 0; j < n; j++) {
        after[j] = kobj_mmio_pa_claimed(pa[j], PAGE_SIZE);
    }

    TEST_ASSERT(n >= 1, "claim assigned no BAR to check");
    for (int j = 0; j < n; j++) {
        TEST_ASSERT(before[j], "an assigned BAR PA was not claimed while the KObj_PCI was alive");
        TEST_ASSERT(!after[j], "a BAR PA stayed claimed after the last unref (claim leak)");
    }
}

// The live count returns to its baseline after a claim + unref (no leak).
void test_pci_live_count_balances(void) {
    if (!rng_pci_present()) return;

    u64 base = kobj_pci_live_count();
    struct KObj_PCI *k = kobj_pci_claim(VIRTIO_DEVICE_ID_RNG);
    u64 during = kobj_pci_live_count();
    if (k) kobj_pci_unref(k);
    u64 after = kobj_pci_live_count();

    TEST_ASSERT(k != NULL, "rng-pci claim failed");
    TEST_ASSERT(during == base + 1, "live count did not increase by 1 on claim");
    TEST_ASSERT(after == base, "live count did not return to baseline on unref (leak)");
}

// SYS_PCI_* validation paths that need no minted handle (always clean -- no
// claim slot taken, no handle leaked). Runs in kproc, which holds CAP_HW_CREATE,
// so the cap gate passes and these reject on the *argument* checks.
void test_pci_syscall_reject(void) {
    // device_id wider than u32 -> rejected before any claim (no slot taken).
    TEST_ASSERT(sys_pci_claim_handler(0x100000000ull, 0) < 0,
                "SYS_PCI_CLAIM device_id > u32 must be rejected");
    // an absent device-id -> no matching function -> -1 (kobj_pci_claim NULLs).
    TEST_ASSERT(sys_pci_claim_handler(PCI_TEST_ABSENT_DEVICE_ID, 0) < 0,
                "SYS_PCI_CLAIM of an absent device-id must fail");
    // MAP_BAR / INFO with a bad fd -> handle_get fails -> -1.
    TEST_ASSERT(sys_pci_map_bar_handler(99999, 0x40000000ull, 0, VMA_PROT_READ) < 0,
                "SYS_PCI_MAP_BAR with a bad fd must fail");
    TEST_ASSERT(sys_pci_info_handler(99999, 0x40000000ull) < 0,
                "SYS_PCI_INFO with a bad fd must fail");
}

// The CLAIM -> mint -> inspect -> validation round-trip through the syscall
// handlers. kproc holds CAP_HW_CREATE, so the claim mints a real KOBJ_PCI fd;
// we inspect its kind + rights, exercise the MAP_BAR / INFO reject paths (all
// of which return BEFORE any burrow-map / copy-out, so no kproc address-space
// pollution + no user buffer needed), then RELEASE before asserting (TEST_ASSERT
// returns on failure -- releasing first never strands the live claim).
void test_pci_syscall_claim_info(void) {
    if (!rng_pci_present()) return;

    struct Proc *p = current_thread()->proc;

    s64  fd     = sys_pci_claim_handler(VIRTIO_DEVICE_ID_RNG, 0);
    bool minted = (fd >= 0);

    bool got    = false;
    int  kind   = -1;
    u64  rights = 0;
    if (minted) {
        struct Handle hh;
        if (handle_get(p, (hidx_t)fd, &hh) == 0) {
            got    = true;
            kind   = (int)hh.kind;
            rights = (u64)hh.rights;
            handle_put(&hh);
        }
    }

    // All four reject before touching a BAR mapping / the user buffer.
    s64 info_badva = minted ? sys_pci_info_handler((u64)fd, 0) : -1;                 // NULL info_va
    s64 map_oob    = minted ? sys_pci_map_bar_handler((u64)fd, 0x40000000ull,
                                                      PCI_BAR_COUNT, VMA_PROT_READ) : -1;  // OOB bar
    s64 map_prot0  = minted ? sys_pci_map_bar_handler((u64)fd, 0x40000000ull, 0, 0) : -1;  // prot==0
    s64 map_wonly  = minted ? sys_pci_map_bar_handler((u64)fd, 0x40000000ull, 0,
                                                      VMA_PROT_WRITE) : -1;          // W without R

    if (minted) handle_close(p, (hidx_t)fd);   // release BEFORE asserting

    TEST_ASSERT(minted, "SYS_PCI_CLAIM(rng) must mint a handle (kproc holds CAP_HW_CREATE)");
    TEST_ASSERT(got, "the minted fd must resolve to a live handle");
    TEST_EXPECT_EQ(kind, (int)KOBJ_PCI, "minted handle must be KOBJ_PCI");
    TEST_EXPECT_EQ(rights, (u64)(RIGHT_READ | RIGHT_WRITE | RIGHT_MAP),
                   "PCI claim mints exactly R|W|MAP (no TRANSFER -- I-5)");
    TEST_ASSERT(info_badva < 0, "SYS_PCI_INFO with a NULL info_va must fail");
    TEST_ASSERT(map_oob   < 0, "SYS_PCI_MAP_BAR with an out-of-range bar index must fail");
    TEST_ASSERT(map_prot0 < 0, "SYS_PCI_MAP_BAR with prot==0 must fail");
    TEST_ASSERT(map_wonly < 0, "SYS_PCI_MAP_BAR with W-without-R must fail");
}
