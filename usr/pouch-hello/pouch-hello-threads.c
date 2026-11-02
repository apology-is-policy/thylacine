// /pouch-hello-threads — the multi-thread pouch hello (Phase 6 sub-chunk 9b).
//
// Closes POUCH-DESIGN.md §13's exit criterion:
//   "A multithreaded test program — N threads, a shared mutex-protected
//    counter, join — runs correctly under default + TSan."
//
// First POSIX C program Thylacine runs that exercises:
//   - pthread_create / pthread_join                        (SYS_THREAD_SPAWN +
//                                                           SYS_TORPOR_WAIT on
//                                                           detach_state)
//   - pthread_mutex_lock / pthread_mutex_unlock            (SYS_TORPOR_WAIT
//                                                           on the mutex word;
//                                                           SYS_TORPOR_WAKE on
//                                                           unlock when there
//                                                           are waiters)
//   - SYS_THREAD_EXIT exit-time clear_child_tid handoff    (the join's wait
//                                                           on &detach_state
//                                                           wakes once the
//                                                           kernel zeroes
//                                                           &__thread_list_lock
//                                                           AND musl's
//                                                           __pthread_exit
//                                                           a_stores
//                                                           DT_EXITED +
//                                                           __wake's it)
//   - the per-thread user stack (allocated by pouch via SYS_BURROW_ATTACH
//                                in mmap-shape; PROT_READ|PROT_WRITE; the
//                                kernel chooses the VA)
//
// Design: 5 worker threads, each takes the mutex and increments a u64
// counter ITER_PER_THREAD times, then exits. Main joins all 5 and
// verifies counter == NTHREADS * ITER_PER_THREAD. If the lock-and-
// increment is broken (race on counter), the final count drops; the
// boot-log integer triggers joey's content-check fail path.
//
// fd 1 is the pipe joey relays to the boot-log UART. Output:
//   pouch-hello-threads: 5 threads, 1000 iters each
//   pouch-hello-threads: counter = 5000 (expected 5000)
//   pouch-hello-threads: ok
//
// Return non-zero on mismatch — joey treats it as a regression.
//
// B-1b (the Pouch memory seam) adds three legs. main-stack-8mib: the initial
// thread's stack is at least 8 MiB (decision 4 of the browser arc; libc derives
// the extent from the kernel's AT_STACK_BASE / AT_STACK_SIZE auxv pair -- patch
// 0045 -- and the maps-row equality pins it against the kernel's own statement).
// deep-frame: a 4 MiB frame touched at both ends, fatal under the old 1 MiB
// stack. guard-vma: every worker finds, in /proc/<pid>/maps, a `---p` row of
// exactly its guardsize immediately below its own stack -- the guard VMA exists,
// the positive control beside /pouch-hello-guard, which proves that it faults.
//
// Three legs read the kernel's `threads:` count in /proc/<pid>/status. The kernel
// frees an exited thread when its CPU switches away from it (ARCH 7.3.2), so a
// joined thread is counted only while it finishes on its CPU. held-counted runs
// first, before any other thread exists: workers held behind a mutex are all
// counted -- the reading sees the workers at all. exited-freed then joins them
// and, creating nothing, waits for the count to fall back to main alone; a
// kernel that freed exited threads only at a create would leave all of them
// counted. join-churn starts and joins more workers than the kernel's per-Proc
// thread cap, one after another: a joined thread that stayed counted refuses a
// user's create at the cap, and joey's run (the system principal, which the cap
// exempts) sees it only in the count.
//
// mask: a new thread starts with its creator's signal mask (POSIX). The kernel
// starts the thread with the mask pthread_create blocked across the spawn, and
// the thread's start restores its creator's own (patch 0065). A thread made while
// main blocks SIGINT reads SIGINT blocked; the control, one variable away, is a
// thread made while SIGINT is open, which reads it open. Both read the mask through
// pthread_sigmask, which answers from the thread's shadow, so two more legs reach
// what that cannot. mask-c11: the same pair through thrd_create, whose start is
// patched separately. mask-queued: the kernel half -- main catches SIGINT, blocks
// it and raises it, so it waits in the queue; a thread is made and joined; main
// unblocks, and the handler must run on main. A newborn the kernel started
// unmasked would take the queued SIGINT at its first entry instead. It posts with
// kill(getpid()), which is process-directed: raise waits on its own thread, which
// no newborn can take from, and the leg would prove nothing.
//
// kill-worker: pthread_kill names one thread. A worker spins at EL0 and main posts
// at it: the handler runs on the worker. A process-directed post runs it on main,
// at the tail of the pthread_kill itself. kill-masked: the worker blocks SIGINT and
// main posts at it. Main stays open and runs several syscalls, and nothing may
// run until the worker unblocks, when the handler runs on the worker before its
// unblock returns. raise-masked: the same with the worker raising at itself, the
// POSIX pthread_kill(pthread_self()): open main must leave it alone. kill-blocked:
// the worker is in nanosleep when the note is posted; it takes it no later than
// the call's end (a native wait rides a caught note out, item 11c, and this leg
// does not pin which). kill-answers: signal 0 probes and posts nothing, a signal
// with no note is EINVAL (SIGUSR1 has none yet), and a tty signal is EPERM.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <threads.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NTHREADS         5u
#define ITER_PER_THREAD  1000u
#define HELD_THREADS     4u
#define CHURN_THREADS    300u    // past the old fixed cap of 256; the count is checked, not a cap
#define FREED_WAIT_MS    5000u

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_hold = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_counter = 0;
static int g_guard_ok[NTHREADS];

static void *held_worker(void *arg) {
    pthread_mutex_lock(&g_hold);
    pthread_mutex_unlock(&g_hold);
    return arg;
}

static void *churn_worker(void *arg) {
    return arg;
}

// The kernel's `threads:` line of this Proc's /proc/<pid>/status; -1 when absent.
static void *mask_worker(void *arg) {
    sigset_t cur;
    sigemptyset(&cur);
    *(int *)arg = pthread_sigmask(SIG_BLOCK, NULL, &cur) == 0 ? sigismember(&cur, SIGINT) : -1;
    return NULL;
}

// What a thread made with SIGINT blocked (or open) sees of SIGINT: 1 blocked,
// 0 open, negative on a failed call. Main's own mask is restored after.
static int mask_seen(int block) {
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGINT);
    if (pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &one, &old) != 0) return -2;
    int seen = -3;
    pthread_t t;
    if (pthread_create(&t, NULL, mask_worker, &seen) != 0 || pthread_join(t, NULL) != 0)
        seen = -4;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return seen;
}

static int mask_worker_c11(void *arg) {
    mask_worker(arg);
    return 0;
}

static int mask_seen_c11(int block) {
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGINT);
    if (pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &one, &old) != 0) return -2;
    int seen = -3;
    thrd_t t;
    if (thrd_create(&t, mask_worker_c11, &seen) != thrd_success ||
        thrd_join(t, NULL) != thrd_success)
        seen = -4;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return seen;
}

static pthread_t g_mask_main;
static volatile int g_mask_on_main = -1;   // 1 the handler ran on main, 0 elsewhere

static void mask_handler(int sig) {
    (void)sig;
    g_mask_on_main = pthread_equal(pthread_self(), g_mask_main) ? 1 : 0;
}

static void *mask_idle(void *arg) {
    (void)arg;
    return NULL;
}

// Where a SIGINT queued while main blocked it is handled once a thread has been
// made and joined and main unblocks: 1 on main, 0 on another thread, -1 nowhere,
// below -1 a failed call.
static int mask_queued_on_main(void) {
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = mask_handler;
    if (sigaction(SIGINT, &sa, &old_sa) != 0) return -2;
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, SIGINT);
    g_mask_main = pthread_self();
    g_mask_on_main = -1;
    int rc = -3;
    if (pthread_sigmask(SIG_BLOCK, &one, &old) == 0) {
        pthread_t t;
        if (kill(getpid(), SIGINT) != 0)
            rc = -4;
        else if (pthread_create(&t, NULL, mask_idle, NULL) != 0 || pthread_join(t, NULL) != 0)
            rc = -5;
        else
            rc = 0;
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
    sigaction(SIGINT, &old_sa, NULL);
    return rc != 0 ? rc : g_mask_on_main;
}

static pthread_t g_kill_ran_on;
static volatile int g_kill_count;
static volatile int g_kill_ready;
static volatile int g_kill_go;
static volatile int g_kill_stop;
static volatile int g_kill_worker_saw;   // what the worker read after unblocking

static void kill_handler(int sig) {
    (void)sig;
    g_kill_ran_on = pthread_self();
    __atomic_add_fetch(&g_kill_count, 1, __ATOMIC_SEQ_CST);
}

static void *kill_spinner(void *arg) {
    (void)arg;
    __atomic_store_n(&g_kill_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_kill_stop, __ATOMIC_SEQ_CST)) { }
    return NULL;
}

// Blocks SIGINT and waits, at EL0, for main to say go, then unblocks and reads the
// count its own handler must already have moved. With arg non-NULL it raises first.
static void *kill_masked_worker(void *arg) {
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &one, NULL) != 0) {
        __atomic_store_n(&g_kill_ready, -1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    if (arg && raise(SIGINT) != 0) {
        __atomic_store_n(&g_kill_ready, -1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    __atomic_store_n(&g_kill_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_kill_go, __ATOMIC_SEQ_CST)) { }
    pthread_sigmask(SIG_UNBLOCK, &one, NULL);
    g_kill_worker_saw = __atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST);
    return NULL;
}

static void *kill_sleeper(void *arg) {
    (void)arg;
    __atomic_store_n(&g_kill_ready, 1, __ATOMIC_SEQ_CST);
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 150 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    return NULL;
}

// Wait, in syscalls, for ready to move: bounded, so a dead worker fails the leg.
static int kill_wait_ready(void) {
    for (int i = 0; i < 2000; i++) {
        if (__atomic_load_n(&g_kill_ready, __ATOMIC_SEQ_CST) != 0)
            return __atomic_load_n(&g_kill_ready, __ATOMIC_SEQ_CST);
        usleep(1000);
    }
    return 0;
}

static void kill_reset(void) {
    __atomic_store_n(&g_kill_count, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_kill_ready, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_kill_go, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_kill_stop, 0, __ATOMIC_SEQ_CST);
    g_kill_worker_saw = -1;
}

// Each leg answers 0, or the line it should fail with on stdout and a non-zero.
static int kill_leg_worker(void) {
    kill_reset();
    pthread_t w;
    if (pthread_create(&w, NULL, kill_spinner, NULL) != 0) return -1;
    int ready = kill_wait_ready();
    int rc = ready == 1 ? pthread_kill(w, SIGINT) : -2;
    for (int i = 0; rc == 0 && i < 3000 && !__atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST); i++)
        usleep(1000);
    int count = __atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST);
    int on_worker = count > 0 && pthread_equal(g_kill_ran_on, w);
    int on_main = count > 0 && pthread_equal(g_kill_ran_on, pthread_self());
    __atomic_store_n(&g_kill_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(w, NULL);
    if (rc != 0 || count != 1 || !on_worker) {
        printf("pouch-hello-threads: kill-worker: rc=%d count=%d on_worker=%d on_main=%d, want 0 1 1 0 (FAIL)\n",
               rc, count, on_worker, on_main);
        return 1;
    }
    return 0;
}

// raises: the worker posts at itself. Otherwise main posts at the worker.
static int kill_leg_masked(int raises, const char *leg) {
    kill_reset();
    pthread_t w;
    if (pthread_create(&w, NULL, kill_masked_worker, raises ? (void *)1 : NULL) != 0) return -1;
    int ready = kill_wait_ready();
    int rc = ready != 1 ? -2 : raises ? 0 : pthread_kill(w, SIGINT);
    // Main is open through all of this: each sleep ends in a tail that would take
    // a note posted anywhere main could reach it.
    for (int i = 0; i < 20; i++) usleep(2000);
    int held = __atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_kill_go, 1, __ATOMIC_SEQ_CST);
    pthread_join(w, NULL);
    int count = __atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST);
    int on_worker = count > 0 && pthread_equal(g_kill_ran_on, w);
    int saw = g_kill_worker_saw;
    if (rc != 0 || held != 0 || count != 1 || !on_worker || saw != 1) {
        printf("pouch-hello-threads: %s: rc=%d held=%d count=%d on_worker=%d saw=%d, want 0 0 1 1 1 (FAIL)\n",
               leg, rc, held, count, on_worker, saw);
        return 1;
    }
    return 0;
}

static int kill_leg_blocked(void) {
    kill_reset();
    pthread_t w;
    if (pthread_create(&w, NULL, kill_sleeper, NULL) != 0) return -1;
    int ready = kill_wait_ready();
    usleep(20 * 1000);
    int rc = ready == 1 ? pthread_kill(w, SIGINT) : -2;
    pthread_join(w, NULL);
    int count = __atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST);
    int on_worker = count > 0 && pthread_equal(g_kill_ran_on, w);
    if (rc != 0 || count != 1 || !on_worker) {
        printf("pouch-hello-threads: kill-blocked: rc=%d count=%d on_worker=%d, want 0 1 1 (FAIL)\n",
               rc, count, on_worker);
        return 1;
    }
    return 0;
}

static int kill_leg_answers(void) {
    kill_reset();
    pthread_t w;
    if (pthread_create(&w, NULL, kill_spinner, NULL) != 0) return -1;
    int ready = kill_wait_ready();
    int probe = pthread_kill(w, 0);
    int no_range = pthread_kill(w, 999);
    int no_note = pthread_kill(w, SIGUSR1);
    int tty = pthread_kill(w, SIGTSTP);
    usleep(20 * 1000);
    int count = __atomic_load_n(&g_kill_count, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_kill_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(w, NULL);
    if (ready != 1 || probe != 0 || no_range != EINVAL || no_note != EINVAL || tty != EPERM || count != 0) {
        printf("pouch-hello-threads: kill-answers: ready=%d probe=%d range=%d usr1=%d tstp=%d count=%d, want 1 0 %d %d %d 0 (FAIL)\n",
               ready, probe, no_range, no_note, tty, count, EINVAL, EINVAL, EPERM);
        return 1;
    }
    return 0;
}

static long status_threads(void) {
    char path[64], line[128];
    long n = -1;
    snprintf(path, sizeof path, "/proc/%d/status", (int)getpid());
    FILE *sf = fopen(path, "r");
    if (!sf) return -1;
    while (fgets(line, sizeof line, sf))
        if (sscanf(line, "threads: %ld", &n) == 1) break;
    fclose(sf);
    return n;
}

// B-1b: the worker's own guard VMA. libc reports the stack as [stackaddr,
// stackaddr + size) with guardsize bytes below it that musl minted PROT_NONE
// (the whole mapping is minted PROT_NONE and the usable part raised, patch
// 0044); the kernel's maps file must show that guard as a `---p` row ending
// exactly at stackaddr and exactly guardsize long -- one mapping, two prots.
static int guard_row_below_self(void) {
    pthread_attr_t at;
    void *addr = NULL;
    size_t size = 0, guard = 0;
    if (pthread_getattr_np(pthread_self(), &at) != 0 ||
        pthread_attr_getstack(&at, &addr, &size) != 0 ||
        pthread_attr_getguardsize(&at, &guard) != 0 || guard == 0)
        return 0;
    uintptr_t hi = (uintptr_t)addr, lo = hi - guard;
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)getpid());
    FILE *mf = fopen(path, "r");
    if (!mf) return 0;
    int found = 0;
    while (fgets(line, sizeof line, mf)) {
        unsigned long a = 0, b = 0;
        char perms[8] = "";
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        if (a == lo && b == hi && strcmp(perms, "---p") == 0) found++;
    }
    fclose(mf);
    return found == 1;
}

// B-1b: a frame deeper than the old 1 MiB stack, touched at both ends. Under
// the 8 MiB mapping both touches fault pages in; under 1 MiB the low touch
// lands in the guard page below the stack and the process dies of a snare.
__attribute__((noinline)) static int deep_frame(void) {
    volatile char frame[4u << 20];
    frame[0] = 1;
    frame[sizeof frame - 1] = 2;
    return frame[0] + frame[sizeof frame - 1];
}

// F14 audit close: error diagnostics use stdout (fd 1, pipe-relayed by joey
// + UART) rather than stderr (fd 2, not installed in joey's pouch-smoke
// spawn). Without this, a failure-mode diagnostic would be silently dropped
// and the user would see only the non-zero exit status without any clue
// what went wrong.
static void *worker(void *arg) {
    unsigned idx = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < ITER_PER_THREAD; i++) {
        if (pthread_mutex_lock(&g_mtx) != 0) {
            printf("pouch-hello-threads: pthread_mutex_lock failed\n");
            fflush(stdout);
            _Exit(2);
        }
        g_counter++;
        if (pthread_mutex_unlock(&g_mtx) != 0) {
            printf("pouch-hello-threads: pthread_mutex_unlock failed\n");
            fflush(stdout);
            _Exit(3);
        }
    }
    g_guard_ok[idx] = guard_row_below_self();
    return NULL;
}

// The kernel's own statement of the initial stack: the row of /proc/<pid>/maps whose
// role column reads `stack` (kernel/devproc.c format_maps). Exactly one such row, or
// the answer is refused -- a second one would mean the role no longer names one mapping.
static int kernel_stack_row(uintptr_t *lo, uintptr_t *hi) {
    char path[64], line[256];
    int found = 0;
    snprintf(path, sizeof path, "/proc/%d/maps", (int)getpid());
    FILE *mf = fopen(path, "r");
    if (!mf) return -1;
    while (fgets(line, sizeof line, mf)) {
        size_t n = strlen(line);
        unsigned long a = 0, b = 0;
        if (n < 7 || strcmp(line + n - 7, " stack\n") != 0) continue;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2 || a >= b) continue;
        *lo = a;
        *hi = b;
        found++;
    }
    fclose(mf);
    return found == 1 ? 0 : -1;
}

int main(void) {
    printf("pouch-hello-threads: %u threads, %u iters each\n",
           NTHREADS, ITER_PER_THREAD);
    fflush(stdout);

    {
        pthread_t held[HELD_THREADS];
        pthread_mutex_lock(&g_hold);
        for (unsigned i = 0; i < HELD_THREADS; i++) {
            int rc = pthread_create(&held[i], NULL, held_worker, NULL);
            if (rc != 0) {
                printf("pouch-hello-threads: exited-counted: pthread_create[%u] failed: %d\n",
                       i, rc);
                fflush(stdout);
                return 13;
            }
        }
        long counted = status_threads();
        if (counted != (long)HELD_THREADS + 1) {
            printf("pouch-hello-threads: held-counted: %ld threads counted with %u workers held, want %u (FAILED)\n",
                   counted, HELD_THREADS, HELD_THREADS + 1u);
            fflush(stdout);
            return 15;
        }
        printf("pouch-hello-threads: held-counted: %u held workers counted OK\n",
               HELD_THREADS);
        pthread_mutex_unlock(&g_hold);
        for (unsigned i = 0; i < HELD_THREADS; i++) {
            int rc = pthread_join(held[i], NULL);
            if (rc != 0) {
                printf("pouch-hello-threads: exited-freed: pthread_join[%u] failed: %d\n",
                       i, rc);
                fflush(stdout);
                return 14;
            }
        }
        unsigned waited_ms = 0;
        for (;;) {
            counted = status_threads();
            if (counted == 1 || waited_ms >= FREED_WAIT_MS) break;
            usleep(1000);
            waited_ms++;
        }
        if (counted != 1) {
            printf("pouch-hello-threads: exited-freed: %ld threads counted %u ms after %u were joined and none created, want 1 (FAILED)\n",
                   counted, waited_ms, HELD_THREADS);
            fflush(stdout);
            return 20;
        }
        printf("pouch-hello-threads: exited-freed: %u joined workers freed with no create since OK\n",
               HELD_THREADS);
        fflush(stdout);
    }

    // The initial thread's reported stack must CONTAIN a main-thread local and BE the
    // mapping the KERNEL says it made. The kernel's side comes from the `stack` row of
    // /proc/<pid>/maps at run time: comparing libc against literals written here would
    // only compare two hand-kept mirrors of exec.h with each other, and both could be
    // stale together.
    {
        pthread_attr_t at;
        void *base = 0;
        size_t size = 0;
        int local = 0;
        uintptr_t klo = 0, khi = 0;
        if (pthread_getattr_np(pthread_self(), &at) != 0 ||
            pthread_attr_getstack(&at, &base, &size) != 0) {
            printf("pouch-hello-threads: main stack query FAILED\n");
            fflush(stdout);
            return 7;
        }
        if (kernel_stack_row(&klo, &khi) != 0) {
            printf("pouch-hello-threads: no single `stack` row in /proc/%d/maps\n",
                   (int)getpid());
            fflush(stdout);
            return 9;
        }
        uintptr_t lo = (uintptr_t)base, hi = lo + size, here = (uintptr_t)&local;
        if (here < lo || here >= hi || lo != klo || hi != khi) {
            printf("pouch-hello-threads: main stack WRONG: libc [%p, %p) kernel [%p, %p) local=%p\n",
                   base, (void *)hi, (void *)klo, (void *)khi, (void *)here);
            fflush(stdout);
            return 8;
        }
        printf("pouch-hello-threads: main stack [%p, %p) size=%lu == kernel maps row OK\n",
               base, (void *)hi, (unsigned long)size);
        fflush(stdout);
        // B-1b decision 4: 8 MiB, derived from the kernel's auxv pair.
        if (size < (8ul << 20)) {
            printf("pouch-hello-threads: main stack is %lu bytes, below the 8 MiB of B-1b\n",
                   (unsigned long)size);
            fflush(stdout);
            return 10;
        }
    }

    if (deep_frame() != 3) {
        printf("pouch-hello-threads: deep frame WRONG\n");
        fflush(stdout);
        return 11;
    }
    printf("pouch-hello-threads: a 4 MiB frame touched at both ends OK\n");
    fflush(stdout);

    pthread_t tids[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; i++) {
        int rc = pthread_create(&tids[i], NULL, worker, (void *)(uintptr_t)i);
        if (rc != 0) {
            printf("pouch-hello-threads: pthread_create[%u] failed: %d\n",
                   i, rc);
            fflush(stdout);
            return 4;
        }
    }

    // Join every worker. pthread_join blocks on each thread's detach_state
    // via __timedwait/__wait → SYS_TORPOR_WAIT. The thread's __pthread_exit
    // tail wakes the joiner via __wake → SYS_TORPOR_WAKE on detach_state.
    for (unsigned i = 0; i < NTHREADS; i++) {
        void *ret = NULL;
        int rc = pthread_join(tids[i], &ret);
        if (rc != 0) {
            printf("pouch-hello-threads: pthread_join[%u] failed: %d\n",
                   i, rc);
            fflush(stdout);
            return 5;
        }
    }

    unsigned long long expected = (unsigned long long)NTHREADS * ITER_PER_THREAD;
    printf("pouch-hello-threads: counter = %llu (expected %llu)\n",
           g_counter, expected);
    if (g_counter != expected) {
        printf("pouch-hello-threads: COUNT MISMATCH (race?)\n");
        fflush(stdout);
        return 6;
    }
    unsigned guarded = 0;
    for (unsigned i = 0; i < NTHREADS; i++) guarded += g_guard_ok[i] ? 1u : 0u;
    if (guarded != NTHREADS) {
        printf("pouch-hello-threads: only %u/%u workers found a ---p row of exactly guardsize below their stack\n",
               guarded, NTHREADS);
        fflush(stdout);
        return 12;
    }
    printf("pouch-hello-threads: %u/%u workers found their ---p guard row in /proc/%d/maps\n",
           NTHREADS, NTHREADS, (int)getpid());
    fflush(stdout);

    // A joined worker can still be finishing on its CPU. Each one counted
    // besides main is on another CPU, or in the switch that frees it there, so
    // at most one per other CPU is left counted.
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) {
        printf("pouch-hello-threads: join-churn: sysconf(_SC_NPROCESSORS_ONLN) = %ld FAILED\n",
               ncpu);
        fflush(stdout);
        return 16;
    }
    for (unsigned i = 0; i < CHURN_THREADS; i++) {
        pthread_t t;
        int rc = pthread_create(&t, NULL, churn_worker, NULL);
        if (rc != 0) {
            printf("pouch-hello-threads: join-churn: pthread_create[%u] failed: %d\n",
                   i, rc);
            fflush(stdout);
            return 17;
        }
        rc = pthread_join(t, NULL);
        if (rc != 0) {
            printf("pouch-hello-threads: join-churn: pthread_join[%u] failed: %d\n",
                   i, rc);
            fflush(stdout);
            return 18;
        }
    }
    long left = status_threads();
    if (left < 1 || left > ncpu) {
        printf("pouch-hello-threads: join-churn: %ld threads counted after %u were joined, want 1..%ld (FAILED)\n",
               left, CHURN_THREADS, ncpu);
        fflush(stdout);
        return 19;
    }
    printf("pouch-hello-threads: join-churn: %u workers started and joined one after another, %ld still counted (1..%ld) OK\n",
           CHURN_THREADS, left, ncpu);
    {
        int blocked = mask_seen(1);
        int open = mask_seen(0);
        if (blocked != 1 || open != 0) {
            printf("pouch-hello-threads: mask: a thread made with SIGINT blocked reads %d, with it open %d, want 1 and 0 (FAILED)\n",
                   blocked, open);
            fflush(stdout);
            return 21;
        }
        printf("pouch-hello-threads: mask: a new thread starts with its creator's mask OK\n");
        int c11_blocked = mask_seen_c11(1);
        int c11_open = mask_seen_c11(0);
        if (c11_blocked != 1 || c11_open != 0) {
            printf("pouch-hello-threads: mask-c11: a thrd_create thread made with SIGINT blocked reads %d, with it open %d, want 1 and 0 (FAILED)\n",
                   c11_blocked, c11_open);
            fflush(stdout);
            return 22;
        }
        printf("pouch-hello-threads: mask-c11: a thrd_create thread starts with its creator's mask OK\n");
        int on_main = mask_queued_on_main();
        if (on_main != 1) {
            printf("pouch-hello-threads: mask-queued: a SIGINT queued while main blocked it was handled %s (%d), want on main (FAILED)\n",
                   on_main == 0 ? "on another thread" : on_main == -1 ? "nowhere" : "after a failed call",
                   on_main);
            fflush(stdout);
            return 23;
        }
        printf("pouch-hello-threads: mask-queued: a queued SIGINT waited for main past a thread's birth OK\n");
    }
    {
        struct sigaction sa, old_sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = kill_handler;
        if (sigaction(SIGINT, &sa, &old_sa) != 0) {
            printf("pouch-hello-threads: kill-worker: sigaction failed: %d\n", errno);
            fflush(stdout);
            return 24;
        }
        int bad = kill_leg_worker();
        if (bad == 0) {
            printf("pouch-hello-threads: kill-worker: pthread_kill ran the handler on the named worker OK\n");
            bad = kill_leg_masked(0, "kill-masked");
        }
        if (bad == 0) {
            printf("pouch-hello-threads: kill-masked: a blocked pthread_kill waited for its thread OK\n");
            bad = kill_leg_masked(1, "raise-masked");
        }
        if (bad == 0) {
            printf("pouch-hello-threads: raise-masked: a blocked raise waited on the raiser OK\n");
            bad = kill_leg_blocked();
        }
        if (bad == 0) {
            printf("pouch-hello-threads: kill-blocked: a thread in nanosleep took it no later than the call's end OK\n");
            bad = kill_leg_answers();
        }
        sigaction(SIGINT, &old_sa, NULL);
        if (bad != 0) {
            if (bad < 0) printf("pouch-hello-threads: kill legs: pthread_create failed: %d\n", bad);
            fflush(stdout);
            return 25;
        }
        printf("pouch-hello-threads: kill-answers: probe, no-note and tty answers OK\n");
    }
    printf("pouch-hello-threads: ok (%u workers, mutex-protected counter, joined)\n",
           NTHREADS);
    /* The census is what joey matches: a stale binary prints the old marker. */
    puts(POUCH_CENSUS_THREADS);
    return 0;
}
