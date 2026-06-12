// PL031 real-time clock -- read once at boot to anchor CLOCK_REALTIME.
//
// Per ARCHITECTURE.md §22.6 (wall clock + monotonic time) + §22.2 (DTB-driven
// hardware discovery). The ARM PrimeCell PL031 (DDI 0224) is the QEMU virt
// real-time clock; its 32-bit Data Register (RTCDR, offset 0x000) reads the
// current time as seconds since the Unix epoch (QEMU loads it from the host
// clock at reset). The kernel reads it EXACTLY ONCE at boot to seed the
// wall-clock anchor (timer_set_wallclock_anchor); thereafter CLOCK_REALTIME
// advances off the fast monotonic counter (CNTVCT), never re-reading the RTC.
//
// This is a read-only v1.0 surface: no clock-set, no alarm IRQ. A settable
// clock / NTP-maintained UTC is the recorded v1.x seam (ARCH §22.6).

#ifndef THYLACINE_ARCH_ARM64_RTC_H
#define THYLACINE_ARCH_ARM64_RTC_H

#include <thylacine/types.h>

// Read the wall-clock epoch (Unix seconds) from the PL031 RTC. Discovers the
// device via the DTB ("arm,pl031") with the QEMU-virt fixed-base fallback
// (0x09010000) -- the same I-15 pattern PL011 uses; the fallback is the
// argued exception for the primary v1.0 target. Maps the MMIO via
// mmu_map_mmio and reads RTCDR (offset 0).
//
// Returns the epoch in seconds, or 0 when no plausible wall clock is readable:
// no DTB "arm,pl031" node (and a fallback read outside the [2020, 2100) window),
// or a map failure. 0 is the FAIL-SOFT signal: the caller anchors CLOCK_REALTIME
// to the epoch so a 0 yields "1970 + uptime" rather than a fabricated time, and
// CLOCK_MONOTONIC is unaffected.
//
// Fail-soft scope: an ABSENT node or an IMPLAUSIBLE value (outside the window)
// returns 0 without extincting -- the realistic "no RTC" cases. A MALFORMED DTB
// reg (an unaligned or oversized `pa`/`size`) is a trusted-input violation
// (I-15: the DTB is the source of hardware truth) and may extinct inside
// `mmu_map_mmio`, consistent with the kernel's posture on a corrupt hardware
// description -- the `if (!kva)` map-failure arm only catches the size==0 path,
// which the caller makes unreachable.
//
// Call ONCE at boot, AFTER dtb_init() and AFTER the MMU + vmalloc are up
// (mmu_map_mmio is live) -- i.e. alongside / just after timer_init().
u64 rtc_read_epoch_seconds(void);

#endif // THYLACINE_ARCH_ARM64_RTC_H
