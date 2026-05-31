// /pouch-hello-exitgroup -- exit_group(2) with LIVE peer threads.
//
// The behavioral proof of SYS_EXIT_GROUP's cross-thread shootdown (ARCH §7.9.1,
// invariant I-24; task #809). A multi-thread pouch Proc calls _Exit(0) -- which
// routes through __NR_exit_group -> SYS_EXIT_GROUP -- while two worker threads
// are STILL ALIVE and UN-JOINED.
//
//   - Pre-fix: __NR_exit_group mapped to SYS_EXITS, which EXTINCTED the kernel
//     ("exits with live peer threads") -- the #808-audit F3 hazard that made
//     tools/test.sh flaky (stratumd's _Exit / abort / mallocng-assert at
//     shutdown is exactly this shape).
//   - Post-fix: SYS_EXIT_GROUP cascades termination to both workers, the Proc
//     exits cleanly with status 0, and joey reaps rc == 0.
//
// Two worker modes deliberately exercise BOTH kernel wake paths:
//   - worker_cond: blocks in pthread_cond_wait  -> a torpor (futex) sleeper,
//                  woken by proc_group_terminate's torpor_wake_all_for_proc.
//   - worker_spin: busy-loops in userspace       -> a peer running on another
//                  core, brought down by the reschedule-IPI kick
//                  (smp_resched_others) + the new IRQ-from-EL0 die-check.
//
// The Proc only reaches ZOMBIE once BOTH workers have died, so joey's
// t_wait_pid returning (rc == 0) is itself the proof that the full cascade
// completed -- no peer is left orphaned.
//
// fd 1 is joey's pipe-relayed UART. The marker line is printed BEFORE
// _Exit (nothing runs after it); joey content-checks that line + reaps rc == 0.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t g_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile int    g_started = 0;

// Block forever in a torpor (futex) sleep -- the cond is never signaled.
static void *worker_cond(void *arg) {
    (void)arg;
    __atomic_add_fetch(&g_started, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&g_mtx);
    for (;;) {
        pthread_cond_wait(&g_cond, &g_mtx);
    }
    return NULL;  // unreachable
}

// Busy-loop in userspace -- a running peer on a core. `yield` is a hint, not a
// syscall, so this stays at EL0 until the reschedule-IPI / timer-tick traps it
// into the kernel, where the IRQ-from-EL0 die-check terminates it.
static void *worker_spin(void *arg) {
    (void)arg;
    __atomic_add_fetch(&g_started, 1, __ATOMIC_SEQ_CST);
    for (;;) {
        __asm__ __volatile__("yield" ::: "memory");
    }
    return NULL;  // unreachable
}

int main(void) {
    pthread_t a, b;
    if (pthread_create(&a, NULL, worker_cond, NULL) != 0 ||
        pthread_create(&b, NULL, worker_spin, NULL) != 0) {
        printf("pouch-hello-exitgroup: pthread_create failed\n");
        fflush(stdout);
        return 4;
    }

    // Spin until BOTH workers are confirmed running, so the _Exit genuinely
    // races LIVE peers (not a single-thread exit before the workers start).
    while (__atomic_load_n(&g_started, __ATOMIC_SEQ_CST) < 2) {
        __asm__ __volatile__("yield" ::: "memory");
    }

    printf("pouch-hello-exitgroup: 2 live un-joined workers (1 cond-wait, 1 spin); calling exit_group(0)\n");
    fflush(stdout);

    // _Exit -> __NR_exit_group -> SYS_EXIT_GROUP. The workers are live; the
    // kernel cascades termination to them and the Proc exits clean (status 0).
    // NEVER returns. (Pre-fix this extincted the kernel on the live peers.)
    _Exit(0);
    return 0;  // unreachable
}
