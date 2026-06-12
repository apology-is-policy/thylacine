// LS-K clock + identity surface (ARCH §22.6).
//
// Covers the new mechanism (the wall-clock anchor + the monotonic/realtime
// derivation) and the 4 syscall handlers. The RTC HARDWARE read
// (rtc_read_epoch_seconds) is exercised at boot (main.c anchors from it) and
// covered TRANSITIVELY here by the realtime-plausibility assertion -- a 0/garbage
// epoch would make the wall clock implausible. We do NOT re-map the PL031 MMIO
// from the test (mmu_map_mmio has no unmap, and re-mapping the same device is an
// unneeded risk); the boot read + this plausibility check are the coverage.

#include "test.h"

#include "../../arch/arm64/timer.h"
#include <thylacine/errno.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// LS-K syscall handlers (non-static in syscall.c; no public header).
extern s64 sys_getpid_handler(u64, u64, u64, u64);
extern s64 sys_getuid_handler(u64, u64, u64, u64);
extern s64 sys_getgid_handler(u64, u64, u64, u64);
extern s64 sys_clock_gettime_handler(u64, u64, u64, u64);

void test_clock_monotonic_advances(void) {
    u64 a = timer_now_ns();
    timer_busy_wait_ticks(2);
    u64 b = timer_now_ns();
    TEST_ASSERT(b > a, "CLOCK_MONOTONIC (timer_now_ns) did not advance");
}

void test_clock_realtime_anchored(void) {
    // CLOCK_REALTIME = CLOCK_MONOTONIC + a non-negative offset (the boot anchor;
    // the RTC epoch dominates the boot-early monotonic, so the offset >= 0). This
    // holds with an RTC (offset > 0) AND without one (fail-soft offset == 0).
    u64 mono = timer_now_ns();
    u64 real = timer_realtime_ns();
    TEST_ASSERT(real >= mono,
        "CLOCK_REALTIME < CLOCK_MONOTONIC (wall-clock offset underflow?)");

    // QEMU virt (the test target) ALWAYS has a PL031 RTC, so the boot read +
    // anchor must have produced a plausible wall clock. A 0 epoch here would be a
    // real RTC-read regression -- NOT the fail-soft path (that is for RTC-less
    // bare metal, where this test target never runs). Window [~2017, ~2096] in
    // seconds: loose enough to never falsely fail on a real host clock, tight
    // enough to catch a 0/garbage epoch (1970 + uptime is << 1.5e9 s).
    u64 real_sec = real / 1000000000ull;
    TEST_ASSERT(real_sec > 1500000000ull,
        "CLOCK_REALTIME is not a plausible wall clock (RTC read returned 0?)");
    TEST_ASSERT(real_sec < 4000000000ull,
        "CLOCK_REALTIME implausibly far in the future (anchor math overflow?)");
}

void test_clock_wallclock_offset_math(void) {
    // Fail-soft: a 0 epoch yields a 0 offset (realtime == monotonic).
    TEST_ASSERT(timer_wallclock_offset_ns(0, 12345) == 0,
        "offset(epoch=0) must be 0 (fail-soft)");
    // offset = epoch_ns - mono_now, no underflow for a plausible epoch.
    TEST_ASSERT(
        timer_wallclock_offset_ns(1600000000ull, 0) == 1600000000ull * 1000000000ull,
        "offset(E, 0) must be E*1e9");
    TEST_ASSERT(
        timer_wallclock_offset_ns(1600000000ull, 1000)
            == 1600000000ull * 1000000000ull - 1000ull,
        "offset(E, M) must be E*1e9 - M");
}

void test_clock_identity_syscalls(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != (void *)0 && t->proc != (void *)0,
        "no current Proc in the test context");
    struct Proc *p = t->proc;

    // The identity reads return the calling Proc's OWN fields, and the right
    // field each (a swap -- getuid returning gid -- is caught when the seeded
    // values differ; even when equal this pins the field each reads).
    TEST_ASSERT(sys_getpid_handler(0, 0, 0, 0) == (s64)p->pid,
        "SYS_GETPID must return the Proc pid");
    TEST_ASSERT(sys_getuid_handler(0, 0, 0, 0) == (s64)(u64)p->principal_id,
        "SYS_GETUID must return principal_id");
    TEST_ASSERT(sys_getgid_handler(0, 0, 0, 0) == (s64)(u64)p->primary_gid,
        "SYS_GETGID must return primary_gid");
}

void test_clock_gettime_errors(void) {
    // A bad clk_id is rejected BEFORE the buffer is touched, so a NULL va is safe.
    TEST_ASSERT(sys_clock_gettime_handler(999, 0, 0, 0) == -T_E_INVAL,
        "SYS_CLOCK_GETTIME with a bad clk_id must return -EINVAL");
    // A valid clk_id with a NULL/invalid user buffer must fault to -EFAULT,
    // never write through the bad VA.
    TEST_ASSERT(sys_clock_gettime_handler(T_CLOCK_MONOTONIC, 0, 0, 0) == -T_E_FAULT,
        "SYS_CLOCK_GETTIME MONOTONIC with a NULL buffer must return -EFAULT");
    TEST_ASSERT(sys_clock_gettime_handler(T_CLOCK_REALTIME, 0, 0, 0) == -T_E_FAULT,
        "SYS_CLOCK_GETTIME REALTIME with a NULL buffer must return -EFAULT");
}
