// P4-Ib: capability machinery tests.
//
// Per <thylacine/caps.h> + specs/handles.tla. kproc is the root of
// trust and starts with CAP_ALL; rfork'd children inherit CAP_NONE
// (Phase 5+ adds parent→child capability delegation via a future
// syscall). Tests pin the kernel-side enforcement that the spec
// invariant HwHandleImpliesCap relies on.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_caps_kproc_has_all(void);
void test_caps_kproc_has_hw_create(void);
void test_caps_rfork_child_has_none(void);
void test_caps_rfork_with_caps_grants_subset(void);
void test_caps_rfork_with_caps_clamps_to_parent(void);
void test_caps_rfork_with_caps_zero_mask(void);
void test_caps_rfork_strips_elevation_only(void);

// kproc starts with CAP_ALL (currently == CAP_HW_CREATE).
void test_caps_kproc_has_all(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc() is NULL");
    TEST_EXPECT_EQ(kp->caps, (u64)CAP_ALL, "kproc->caps != CAP_ALL");
}

// Explicit: kproc has CAP_HW_CREATE specifically (so kernel test code
// can call kobj_mmio_create / kobj_irq_create through the syscall path).
void test_caps_kproc_has_hw_create(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc() is NULL");
    TEST_ASSERT((kp->caps & CAP_HW_CREATE) != 0,
                "kproc lacks CAP_HW_CREATE");
}

// rfork'd children start with CAP_NONE. Verifies the proc_alloc path
// leaves caps at the KP_ZERO default (no implicit inheritance at v1.0).
static int caps_child_observed = -1;
static void caps_child_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t)                          extinction("caps_child: no current_thread");
    struct Proc *p = t->proc;
    if (!p)                          extinction("caps_child: no proc");
    caps_child_observed = (int)p->caps;
    exits("ok");
}

void test_caps_rfork_child_has_none(void) {
    caps_child_observed = -1;
    int pid = rfork(RFPROC, caps_child_thunk, NULL);
    TEST_ASSERT(pid > 0, "rfork failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "child exit status");
    TEST_EXPECT_EQ(caps_child_observed, (int)CAP_NONE,
                   "rfork'd child started with non-empty caps");
}

// P4-Ic3: rfork_with_caps grants the requested subset when the parent
// holds those bits. kproc holds CAP_ALL, so requesting CAP_HW_CREATE
// yields exactly that bit on the child (no more, no less).
void test_caps_rfork_with_caps_grants_subset(void) {
    caps_child_observed = -1;
    int pid = rfork_with_caps(RFPROC, caps_child_thunk, NULL, CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "child exit status");
    TEST_EXPECT_EQ(caps_child_observed, (int)CAP_HW_CREATE,
                   "child did not receive CAP_HW_CREATE");
}

// P4-Ic3: rfork_with_caps cannot grant beyond the parent's caps. A
// grandchild forked from a (zero-caps) child cannot obtain CAP_HW_CREATE
// even when the grandchild's caller passes the bit in caps_mask — the
// AND with parent->caps yields {}. Maps to spec CapsCeiling: the
// child's ceiling is proc_caps[parent] AT fork time; requesting more
// than the parent holds clamps to the intersection. The grandchild
// thunk records its own caps; the child reaps it, then exits.
static int caps_grandchild_observed = -1;
static void caps_grandchild_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t)                          extinction("caps_grandchild: no current_thread");
    struct Proc *p = t->proc;
    if (!p)                          extinction("caps_grandchild: no proc");
    caps_grandchild_observed = (int)p->caps;
    exits("ok");
}
static void caps_intermediate_thunk(void *arg) {
    (void)arg;
    // intermediate has caps = CAP_NONE (rfork'd from kproc).
    // It tries to grant CAP_HW_CREATE to its own child — the AND with
    // its own caps yields CAP_NONE. Spec: granted ⊆ proc_caps[parent].
    int gpid = rfork_with_caps(RFPROC, caps_grandchild_thunk, NULL, CAP_HW_CREATE);
    if (gpid <= 0) extinction("intermediate rfork_with_caps failed");
    int gstatus = -42;
    int greaped = wait_pid(&gstatus);
    if (greaped != gpid) extinction("intermediate wait_pid pid mismatch");
    if (gstatus != 0)    extinction("grandchild exit status non-zero");
    exits("ok");
}
void test_caps_rfork_with_caps_clamps_to_parent(void) {
    caps_grandchild_observed = -1;
    int ipid = rfork(RFPROC, caps_intermediate_thunk, NULL);
    TEST_ASSERT(ipid > 0, "intermediate rfork failed");

    int istatus = -42;
    int ireaped = wait_pid(&istatus);
    TEST_EXPECT_EQ(ireaped, ipid, "intermediate wait_pid pid");
    TEST_EXPECT_EQ(istatus, 0, "intermediate exit status");
    TEST_EXPECT_EQ(caps_grandchild_observed, (int)CAP_NONE,
                   "grandchild received caps its parent didn't hold");
}

// P4-Ic3: rfork_with_caps with mask=CAP_NONE produces the same result
// as plain rfork — the child has CAP_NONE. Pins the equivalence used
// by rfork()'s delegation to rfork_internal(..., CAP_NONE).
void test_caps_rfork_with_caps_zero_mask(void) {
    caps_child_observed = -1;
    int pid = rfork_with_caps(RFPROC, caps_child_thunk, NULL, CAP_NONE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid");
    TEST_EXPECT_EQ(status, 0, "child exit status");
    TEST_EXPECT_EQ(caps_child_observed, (int)CAP_NONE,
                   "child got non-empty caps from CAP_NONE mask");
}

// A-4-pre / I-2: rfork strips CAP_ELEVATION_ONLY from the child. An
// elevated parent (one holding CAP_HOSTOWNER, as if it had redeemed a
// console-gated `cap`-device grant) must NOT leak that bit across a fork
// even when the caller passes a mask that includes it; the fork-grantable
// subset still flows. Maps to caps.h::CAP_ELEVATION_ONLY +
// specs/handles.tla::ElevationOnly. Pre-fix (no ~CAP_ELEVATION_ONLY AND in
// rfork_internal) the grandchild observes CAP_HOSTOWNER and this FAILS.
//
// The intermediate is a throwaway Proc that mutates its OWN caps (nothing
// else reads them) — this avoids touching kproc's live caps, and mirrors
// the clamps_to_parent two-level pattern above.
static int caps_elev_grandchild_observed = -1;
static void caps_elev_grandchild_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t)                          extinction("caps_elev_grandchild: no current_thread");
    struct Proc *p = t->proc;
    if (!p)                          extinction("caps_elev_grandchild: no proc");
    caps_elev_grandchild_observed = (int)p->caps;
    exits("ok");
}
static void caps_elev_intermediate_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    if (!t)                          extinction("caps_elev_intermediate: no current_thread");
    struct Proc *p = t->proc;
    if (!p)                          extinction("caps_elev_intermediate: no proc");
    // Simulate a legitimately-elevated parent holding both the full
    // fork-grantable ceiling AND every elevation-only cap (HOSTOWNER plus
    // the A-4 finer caps DAC_OVERRIDE / CHOWN / KILL).
    p->caps |= (CAP_ALL | CAP_ELEVATION_ONLY);
    // Try to hand the whole set (incl. every elevation-only bit) down.
    int gpid = rfork_with_caps(RFPROC, caps_elev_grandchild_thunk, NULL,
                               CAP_ALL | CAP_ELEVATION_ONLY);
    if (gpid <= 0) extinction("elev intermediate rfork_with_caps failed");
    int gstatus = -42;
    int greaped = wait_pid(&gstatus);
    if (greaped != gpid) extinction("elev intermediate wait_pid pid mismatch");
    if (gstatus != 0)    extinction("elev grandchild exit status non-zero");
    exits("ok");
}
void test_caps_rfork_strips_elevation_only(void) {
    caps_elev_grandchild_observed = -1;
    int ipid = rfork(RFPROC, caps_elev_intermediate_thunk, NULL);
    TEST_ASSERT(ipid > 0, "elev intermediate rfork failed");

    int istatus = -42;
    int ireaped = wait_pid(&istatus);
    TEST_EXPECT_EQ(ireaped, ipid, "elev intermediate wait_pid pid");
    TEST_EXPECT_EQ(istatus, 0, "elev intermediate exit status");
    // NO elevation-only bit may cross the fork -- HOSTOWNER and the A-4
    // finer caps alike; the A-4-pre strip auto-covers the whole macro.
    TEST_ASSERT((caps_elev_grandchild_observed & (int)CAP_ELEVATION_ONLY) == 0,
                "rfork leaked an elevation-only cap across fork");
    // ...while the full fork-grantable ceiling (incl. CAP_GRANT_CLEARANCE)
    // still flows intact.
    TEST_EXPECT_EQ(caps_elev_grandchild_observed, (int)CAP_ALL,
                   "rfork did not confer the fork-grantable subset");
}
