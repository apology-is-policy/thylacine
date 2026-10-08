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
#include <thylacine/caps.h>
#include <thylacine/errno.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>

// LS-K syscall handlers (non-static in syscall.c; no public header).
extern s64 sys_getpid_handler(u64, u64, u64, u64);
extern s64 sys_getuid_handler(u64, u64, u64, u64);
extern s64 sys_getgid_handler(u64, u64, u64, u64);
extern s64 sys_clock_gettime_handler(u64, u64, u64, u64);
extern s64 sys_clock_settime_handler(u64, u64, u64, u64);   // net-7a

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
    // LS-K audit F4: a small nonzero epoch whose epoch_ns < mono_now must
    // fail-soft to 0 (the public helper must be total). Pre-fix the subtraction
    // wrapped to ~UINT64_MAX; the guard returns 0.
    TEST_ASSERT(timer_wallclock_offset_ns(1ull, 2000000000ull) == 0,
        "offset(epoch_ns < mono) must be 0 (no u64 underflow)");
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

// net-7a: the runtime re-anchor (timer level -- no syscall/uaccess/cap). Steps
// CLOCK_REALTIME to a known epoch + verifies it lands there, then restores the
// boot anchor (projected forward by the elapsed monotonic so the original offset
// is preserved within ns, not frozen) so the later realtime-plausibility checks
// hold regardless of test order. MONOTONIC must not move.
void test_clock_settime_reanchors(void) {
    u64 mono0 = timer_now_ns();
    u64 real0 = timer_realtime_ns();

    // Step to ~2033 (distinct from the boot anchor) and read it back.
    u64 target_sec = 2000000000ull;
    timer_reset_wallclock_anchor_ns(target_sec * 1000000000ull);
    u64 r_sec = timer_realtime_ns() / 1000000000ull;
    TEST_ASSERT(r_sec >= target_sec && r_sec < target_sec + 5,
        "SYS_CLOCK_SETTIME re-anchor did not step CLOCK_REALTIME to the target");

    // A re-anchor must not perturb CLOCK_MONOTONIC.
    u64 m1 = timer_now_ns();
    timer_reset_wallclock_anchor_ns(1ull * 1000000000ull);   // step to ~1970
    u64 m2 = timer_now_ns();
    TEST_ASSERT(m2 >= m1, "re-anchor must not move CLOCK_MONOTONIC");

    // Restore the original wall clock (preserve the offset, do not freeze it).
    timer_reset_wallclock_anchor_ns(real0 + (timer_now_ns() - mono0));
    TEST_ASSERT(timer_realtime_ns() >= timer_now_ns(),
        "restored CLOCK_REALTIME underflowed below MONOTONIC");
}

// net-7a: the SYS_CLOCK_SETTIME cap + clk_id gate, ordered cap-then-buffer. With
// CAP_HOSTOWNER cleared, REALTIME is rejected at the cap gate (-EACCES) BEFORE
// any buffer read (a NULL va never faults first); with it set, the same NULL va
// now faults (-EFAULT) -- proving the cap actually gates and is checked first.
// clk_id is validated before the cap, so MONOTONIC stays EINVAL either way.
void test_clock_settime_cap_gate(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != (void *)0 && t->proc != (void *)0,
        "no current Proc in the test context");
    struct Proc *p = t->proc;
    caps_t saved = __atomic_load_n(&p->caps, __ATOMIC_ACQUIRE);

    // Capture every handler result while the cap is flipped, then restore the
    // cap BEFORE any assert -- so an early assert-abort (TEST_ASSERT returns
    // from the test fn) cannot leak a flipped CAP_HOSTOWNER on this (the test)
    // Proc into a later serially-run test (net-7d audit F3).
    __atomic_store_n(&p->caps, saved & ~(caps_t)CAP_HOSTOWNER, __ATOMIC_RELEASE);
    s64 noown_realtime = sys_clock_settime_handler(T_CLOCK_REALTIME, 0, 0, 0);
    s64 noown_mono     = sys_clock_settime_handler(T_CLOCK_MONOTONIC, 0, 0, 0);
    __atomic_store_n(&p->caps, saved | CAP_HOSTOWNER, __ATOMIC_RELEASE);
    s64 own_realtime = sys_clock_settime_handler(T_CLOCK_REALTIME, 0, 0, 0);
    s64 own_mono     = sys_clock_settime_handler(T_CLOCK_MONOTONIC, 0, 0, 0);
    __atomic_store_n(&p->caps, saved, __ATOMIC_RELEASE);

    TEST_ASSERT(noown_realtime == -T_E_ACCES,
        "SYS_CLOCK_SETTIME without CAP_HOSTOWNER must return -EACCES");
    TEST_ASSERT(noown_mono == -T_E_INVAL,
        "SYS_CLOCK_SETTIME MONOTONIC must be EINVAL (checked before the cap)");
    TEST_ASSERT(own_realtime == -T_E_FAULT,
        "SYS_CLOCK_SETTIME with CAP_HOSTOWNER + NULL buffer must return -EFAULT");
    TEST_ASSERT(own_mono == -T_E_INVAL,
        "SYS_CLOCK_SETTIME MONOTONIC must be EINVAL even with CAP_HOSTOWNER");
}

// ---------------------------------------------------------------------------
// VIVARIUM 6.29: vivarium_clock_sleep, the sleep under the vivarium's
// nanosleep and clock_nanosleep rows. Each leg runs it in a thread of a fresh
// Proc through test_caught_run.
// ---------------------------------------------------------------------------

#define CNS_MS  1000000ull
#define CNS_SEC 1000000000ull

// A Linux leg's request is long enough that no stall between the park and the
// post lets its deadline win, and short enough that a sleeper the note missed
// still returns inside the fixture's 2 s release wait rather than stranding.
// The native control rides its request out, so it is shorter.
#define CNS_LONG (1500 * CNS_MS)
#define CNS_CTL  (1000 * CNS_MS)

static bool g_cns_wall;
static bool g_cns_abs;
static u64  g_cns_req;
static u64  g_cns_rem;

static void cns_set(bool wall, bool abstime, u64 req_ns) {
    g_cns_wall = wall;
    g_cns_abs  = abstime;
    g_cns_req  = req_ns;
    g_cns_rem  = 0;
}

// SIG_DFL: an interrupt posted to a Proc with this disposition arms the
// terminate latch rather than a caught note.
static const struct viv_ksigaction g_cns_sig_dfl = {
    .handler = 0, .flags = 0, .restorer = 0, .mask = 0 };

static long cns_sleep(void *arg) {
    (void)arg;
    return (long)vivarium_clock_sleep(g_cns_wall, g_cns_abs, g_cns_req, &g_cns_rem);
}

// A caught note ends a Linux sleeper before its deadline with EINTR and the time
// left, and a native sleeper rides the note out to its deadline. A deadline
// already past returns 0 at once even with a note pending -- the opposite of
// pause()'s zero timeout -- and an absolute deadline of 0, tsleep's no-deadline
// sentinel, is a past deadline, never a sleep without one.
void test_clock_nanosleep_caught_note(void);
void test_clock_nanosleep_caught_note(void) {
    cns_set(false, false, CNS_LONG);
    struct Proc *lin = test_caught_proc(true);
    u64 l0 = timer_now_ns();
    struct test_caught_leg leg = test_caught_run(lin, cns_sleep, NULL, NULL, NULL, false);
    u64 leg_ns = timer_now_ns() - l0;
    u64 leg_rem = g_cns_rem;
    test_caught_proc_free(lin, &leg);

    cns_set(false, false, CNS_CTL);
    struct Proc *nat = test_caught_proc(false);
    u64 t0 = timer_now_ns();
    struct test_caught_leg ctl = test_caught_run(nat, cns_sleep, NULL, NULL, NULL, false);
    u64 ctl_ns = timer_now_ns() - t0;
    test_caught_proc_free(nat, &ctl);

    cns_set(false, false, CNS_LONG);
    struct Proc *pend = test_caught_proc(true);
    u64 e0 = timer_now_ns();
    struct test_caught_leg early = test_caught_run(pend, cns_sleep, NULL, NULL, NULL, true);
    u64 early_ns = timer_now_ns() - e0;
    u64 early_rem = g_cns_rem;
    test_caught_proc_free(pend, &early);

    cns_set(false, false, 0);
    struct Proc *now = test_caught_proc(true);
    struct test_caught_leg zero = test_caught_run(now, cns_sleep, NULL, NULL, NULL, true);
    test_caught_proc_free(now, &zero);

    cns_set(false, true, 0);
    struct Proc *sen = test_caught_proc(false);
    struct test_caught_leg sentinel = test_caught_run(sen, cns_sleep, NULL, NULL, NULL, false);
    test_caught_proc_free(sen, &sentinel);

    // The interrupt has no handler, so its post arms the terminate latch and
    // the sleep unwinds as a death. A peer could still revoke the latch before
    // the thread's tail, so the sleep must not return 0 short of its deadline.
    cns_set(false, false, CNS_LONG);
    struct Proc *dfl = test_caught_proc(true);
    bool dfl_set = dfl && viv_sigtab_set(dfl->sigtab, VIV_SIGNOTE_INTERRUPT, &g_cns_sig_dfl);
    u64 d0 = timer_now_ns();
    struct test_caught_leg death = test_caught_run(dfl_set ? dfl : NULL, cns_sleep, NULL, NULL,
                                                   NULL, true);
    u64 death_ns = timer_now_ns() - d0;
    u64 death_rem = g_cns_rem;
    test_caught_proc_free(dfl, &death);

    TEST_ASSERT(lin != NULL && nat != NULL && pend != NULL && now != NULL && sen != NULL &&
                dfl_set, "the Procs");
    TEST_ASSERT(leg.parked && leg.posted && leg.joined,
                "the Linux sleeper parked, the note posted, the sleep returned");
    TEST_ASSERT(leg.on_post, "a caught note ends the sleep before its deadline");
    TEST_EXPECT_EQ(leg.rc, -(long)T_E_INTR, "EINTR");
    // rem is the deadline less the clock at the note, and both ends of the
    // sleep lie inside the leg, so it is the request less at most the leg's
    // length: exact bounds, however slow the host.
    TEST_ASSERT(leg_rem > 0 && leg_rem < CNS_LONG,
                "with the time left, which is less than the request");
    TEST_ASSERT(leg_rem + leg_ns >= CNS_LONG,
                "and is the request less at most the time the leg took");
    TEST_ASSERT(ctl.parked && ctl.posted && ctl.joined,
                "control: the native sleeper parked, the note posted, the sleep returned");
    TEST_ASSERT(ctl.rode_out, "control: the note woke the native sleeper and it slept again");
    TEST_EXPECT_EQ(ctl.rc, 0L, "control: the native sleep ends on its deadline");
    TEST_ASSERT(ctl_ns >= CNS_CTL, "control: and not before it");
    TEST_ASSERT(early.posted && early.on_post && early.joined,
                "a note pending at entry ends the sleep at once");
    TEST_EXPECT_EQ(early.rc, -(long)T_E_INTR, "EINTR, the deadline still ahead");
    TEST_ASSERT(early_rem > 0 && early_rem <= CNS_LONG &&
                early_rem + early_ns >= CNS_LONG,
                "with all of the request left but the time the leg took");
    TEST_ASSERT(zero.posted && zero.on_post && zero.joined, "a zero sleep returned at once");
    TEST_EXPECT_EQ(zero.rc, 0L,
                   "a zero sleep with a note pending is 0: the expiry wins (pause's is EINTR)");
    TEST_ASSERT(!sentinel.parked && sentinel.on_post && sentinel.joined,
                "an absolute deadline of 0 has passed: no sleep without a deadline");
    TEST_EXPECT_EQ(sentinel.rc, 0L, "and it returns 0");
    TEST_ASSERT(!death.parked && death.on_post && death.joined,
                "a pending terminate note unwinds the sleep at once");
    TEST_EXPECT_EQ(death.rc, -(long)T_E_INTR,
                   "as EINTR, never a short 0 a surviving thread would read as a full sleep");
    TEST_ASSERT(death_rem > 0 && death_rem <= CNS_LONG && death_rem + death_ns >= CNS_LONG,
                "with the time left");
}

// The wall clock's offset when the step test began; each leg restores it.
static u64 g_cnw_off0;
static u64 g_cnw_old_dl;

static void cnw_restore(void) {
    timer_reset_wallclock_anchor_ns(timer_now_ns() + g_cnw_off0);
}

static bool cnw_step_forward(void *arg) {
    (void)arg;
    timer_reset_wallclock_anchor_ns(timer_realtime_ns() + 60ull * CNS_SEC);
    return true;
}

// Step back, then outlast the monotonic deadline the instant had before the
// step: a sleeper the step did not reach returns at it.
static bool cnw_step_back(void *arg) {
    (void)arg;
    timer_reset_wallclock_anchor_ns(timer_realtime_ns() - 60ull * CNS_SEC);
    u64 dl = g_cnw_old_dl + 200 * CNS_MS;
    while (timer_now_ns() < dl) sched();
    return true;
}

static void cnw_release(void *arg) {
    (void)arg;
    cnw_restore();
}

// TIMER_ABSTIME on CLOCK_REALTIME follows the wall clock (POSIX): a step past
// the instant ends the sleep at once, and a step back sleeps on toward the
// instant's new place. A relative sleep never consults the wall clock, so a step
// leaves it alone. Native Procs: the note the fixture posts is ridden out.
void test_clock_nanosleep_wall_step(void);
void test_clock_nanosleep_wall_step(void) {
    g_cnw_off0 = timer_wallclock_offset_ns_now();

    // The instant is 1.5 s ahead, inside the fixture's 2 s release wait, so a
    // sleeper the step missed returns there late rather than stranded.
    cns_set(true, true, timer_realtime_ns() + 1500 * CNS_MS);
    struct Proc *fp = test_caught_proc(false);
    struct test_caught_leg fwd = test_caught_run(fp, cns_sleep, cnw_step_forward, NULL,
                                                 NULL, false);
    cnw_restore();
    test_caught_proc_free(fp, &fwd);

    cns_set(true, true, timer_realtime_ns() + 1 * CNS_SEC);
    g_cnw_old_dl = timer_now_ns() + 1 * CNS_SEC;
    struct Proc *bp = test_caught_proc(false);
    struct test_caught_leg back = test_caught_run(bp, cns_sleep, cnw_step_back, cnw_release,
                                                  NULL, false);
    cnw_restore();
    test_caught_proc_free(bp, &back);

    cns_set(true, false, 300 * CNS_MS);
    struct Proc *rp = test_caught_proc(false);
    u64 t0 = timer_now_ns();
    struct test_caught_leg rel = test_caught_run(rp, cns_sleep, cnw_step_forward, NULL,
                                                 NULL, false);
    u64 rel_ns = timer_now_ns() - t0;
    cnw_restore();
    test_caught_proc_free(rp, &rel);

    TEST_ASSERT(fp != NULL && bp != NULL && rp != NULL, "the Procs");
    TEST_ASSERT(fwd.parked && fwd.joined, "the absolute sleeper parked, then returned");
    TEST_ASSERT(!fwd.prepped && !fwd.posted && fwd.on_post,
                "a step past the instant ended the sleep at once");
    TEST_EXPECT_EQ(fwd.rc, 0L, "with 0: the instant has passed");
    TEST_ASSERT(back.parked && back.prepped,
                "after a step back the sleeper still slept, past its old deadline");
    TEST_ASSERT(back.posted && back.rode_out && !back.on_post,
                "control: the note was ridden out");
    TEST_ASSERT(back.joined, "the step forward to the true time ended it");
    TEST_EXPECT_EQ(back.rc, 0L, "with 0");
    TEST_ASSERT(rel.parked && rel.prepped, "a relative sleep slept on through the step");
    TEST_ASSERT(rel.joined, "and returned");
    TEST_EXPECT_EQ(rel.rc, 0L, "with 0 on its deadline");
    TEST_ASSERT(rel_ns >= 300 * CNS_MS, "which the step did not bring forward");
}
