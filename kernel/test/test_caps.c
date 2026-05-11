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
