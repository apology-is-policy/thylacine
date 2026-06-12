// PL031 real-time clock -- boot-time wall-clock epoch read (LS-K).
// See rtc.h + ARCHITECTURE.md §22.6.

#include "rtc.h"

#include "mmu.h"

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/page.h>

// PL031 register layout (ARM PrimeCell RTC TRM, ARM DDI 0224).
#define PL031_DR        0x000u   // RTCDR: data register (current count, RO)
#define PL031_MMIO_SIZE 0x1000u  // one 4 KiB page

// QEMU virt fixed base (the documented I-15 fallback, like PL011's
// 0x09000000). Used only when no "arm,pl031" DTB node is present.
#define PL031_FALLBACK_BASE 0x09010000UL

// Plausibility window: [2020-01-01, 2100-01-01). A real PL031 returns the host's
// Unix time (inside this); a dead/absent MMIO slot reads 0 (below the floor) OR
// floats to 0xFFFFFFFF = 4294967295 s = year 2106 (above the ceiling) -- the
// classic undriven-bus all-ones pattern. EITHER side -> "no wall clock" ->
// fail-soft to 0, so CLOCK_REALTIME reads 1970 + uptime (the honest signal)
// instead of a fabricated plausible-but-wrong time. The ceiling closes the
// high-garbage leg the low floor alone missed (LS-K audit F2).
#define RTC_EPOCH_FLOOR   1577836800ull  // 2020-01-01T00:00:00Z
#define RTC_EPOCH_CEILING 4102444800ull  // 2100-01-01T00:00:00Z

u64 rtc_read_epoch_seconds(void) {
    u64 pa = 0, size = 0;
    if (!dtb_get_compat_reg("arm,pl031", &pa, &size) || pa == 0) {
        // No DTB node -> the QEMU-virt fixed base (I-15 documented fallback).
        pa = PL031_FALLBACK_BASE;
        size = PL031_MMIO_SIZE;
    }
    // The DR offset must lie inside the mapped region; a malformed reg size
    // falls back to a full page (the device is one page on every target).
    if (size < (u64)PL031_DR + 4u) size = PL031_MMIO_SIZE;

    void *kva = mmu_map_mmio((paddr_t)pa, (size_t)size);
    if (!kva) return 0;   // map failure -> fail-soft (no wall clock)

    // Single MMIO read. The mapping is held (there is no mmu_unmap_mmio and
    // the kernel never re-reads the RTC at v1.0); one page of vmalloc for a
    // kernel-reserved device (I-5) is harmless.
    u32 secs = *(volatile u32 *)((uintptr_t)kva + PL031_DR);

    if ((u64)secs < RTC_EPOCH_FLOOR || (u64)secs > RTC_EPOCH_CEILING)
        return 0;   // outside [2020, 2100) -> implausible -> fail-soft
    return (u64)secs;
}
