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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS         5u
#define ITER_PER_THREAD  1000u

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_counter = 0;

// F14 audit close: error diagnostics use stdout (fd 1, pipe-relayed by joey
// + UART) rather than stderr (fd 2, not installed in joey's pouch-smoke
// spawn). Without this, a failure-mode diagnostic would be silently dropped
// and the user would see only the non-zero exit status without any clue
// what went wrong.
static void *worker(void *arg) {
    (void)arg;
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
    return NULL;
}

int main(void) {
    printf("pouch-hello-threads: %u threads, %u iters each\n",
           NTHREADS, ITER_PER_THREAD);
    fflush(stdout);

    pthread_t tids[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; i++) {
        int rc = pthread_create(&tids[i], NULL, worker, NULL);
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
    printf("pouch-hello-threads: ok (%u workers, mutex-protected counter, joined)\n",
           NTHREADS);
    printf("pouch-hello-threads: exit 0\n");
    return 0;
}
