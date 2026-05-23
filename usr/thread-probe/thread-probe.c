// /thread-probe — end-to-end test of SYS_THREAD_SPAWN + SYS_THREAD_EXIT
// + the clear-child-tid join handshake (P6-pouch-threads sub-chunk 9a).
//
// Calling convention: stand-alone — joey spawns this binary with no
// inherited fds. It runs entirely in user-mode using libt syscall
// wrappers; no pouch dependencies.
//
// Sequence:
//
//   1. main initialises a static u32 `worker_tidptr` to a known
//      sentinel (0xdeadbeef) and a static u32 `shared_counter` to 0.
//
//   2. main spawns a worker Thread via t_thread_spawn(entry, sp_top,
//      &args, 0):
//        - entry  = worker_entry (a function in this binary, EL0 VA)
//        - sp_top = top of a static .bss-aligned 4 KiB stack
//        - arg    = pointer to a struct {tidptr, counter} so the worker
//                   can find both locations
//        - tls    = 0 (no TLS at v1.0 sub-chunk 9a — pouch's pthread
//                   layer wires TLS at 9c)
//
//   3. Worker:
//        a. Calls t_set_tid_address(&worker_tidptr) — registers the
//           tidptr on the worker Thread. The kernel will atomically
//           clear *worker_tidptr + torpor-wake on Thread exit.
//        b. Atomically stores 42 into *shared_counter (RELEASE).
//        c. Calls t_thread_exit() — never returns.
//
//   4. main calls t_torpor_wait(&worker_tidptr, 0xdeadbeef, -1) to
//      wait until the kernel's exit-time store changes the word.
//        - Three orderings are possible (see torpor.h proof sketch):
//          a. Worker exits BEFORE main's wait. Kernel store + wake
//             happen; main's wait fast-paths (load 0 != 0xdeadbeef,
//             return 0). Re-check confirms value = 0.
//          b. Worker exits DURING main's wait (registered in bucket).
//             Kernel wake delivers; main re-checks the word = 0.
//          c. Spurious wake (impossible at v1.0 — no other waker on
//             this address — but the loop handles it just in case).
//
//   5. main verifies *shared_counter == 42 — the worker ran the body.
//
//   6. main t_putstr("thread-probe: ok\n") + t_exits(0).
//
// On any error: diagnostic + exit 1.

#include <thyla/syscall.h>

#define WORKER_STACK_SIZE 4096
#define TIDPTR_SENTINEL   0xdeadbeefu
#define COUNTER_TARGET    42u

// Static .bss so the worker's stack is in a stable region not subject
// to main's stack-frame layout. 16-byte aligned per AAPCS64.
static unsigned char worker_stack[WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

// Synchronisation words. `worker_tidptr` is the join handshake; the
// kernel clears it to 0 on worker exit. `shared_counter` is the
// worker's "I ran" marker.
static volatile unsigned int worker_tidptr;
static volatile unsigned int shared_counter;

// Argument struct passed to the worker via x0 (AAPCS64). Static so the
// pointer remains valid past main's stack-frame exit-and-recreation.
struct worker_arg {
    volatile unsigned int *tidptr_va;
    volatile unsigned int *counter_va;
};
static struct worker_arg wa;

__attribute__((noreturn))
static void worker_entry(void *arg) {
    struct worker_arg *a = (struct worker_arg *)arg;
    // Register the tidptr so the kernel clear+wake at SYS_THREAD_EXIT
    // delivers to any joiner blocked in torpor_wait.
    (void)t_set_tid_address((void *)a->tidptr_va);
    __atomic_store_n(a->counter_va, COUNTER_TARGET, __ATOMIC_RELEASE);
    t_thread_exit();
}

int main(void) {
    __atomic_store_n(&worker_tidptr,   TIDPTR_SENTINEL, __ATOMIC_RELEASE);
    __atomic_store_n(&shared_counter,  0u,              __ATOMIC_RELEASE);

    wa.tidptr_va  = &worker_tidptr;
    wa.counter_va = &shared_counter;

    void *sp_top = (void *)(worker_stack + WORKER_STACK_SIZE);
    long tid = t_thread_spawn(worker_entry, sp_top, &wa, (void *)0);
    if (tid < 0) {
        t_putstr("thread-probe: t_thread_spawn failed\n");
        t_exits(1);
    }

    // Join: wait until *worker_tidptr is no longer the sentinel.
    for (;;) {
        long rc = t_torpor_wait((unsigned int *)&worker_tidptr,
                                TIDPTR_SENTINEL, -1);
        if (rc != T_TORPOR_OK) {
            t_putstr("thread-probe: torpor_wait returned non-zero\n");
            t_exits(1);
        }
        unsigned int v = __atomic_load_n(&worker_tidptr, __ATOMIC_ACQUIRE);
        if (v == 0u) break;
        if (v != TIDPTR_SENTINEL) {
            t_putstr("thread-probe: tidptr observed unexpected value\n");
            t_exits(1);
        }
        // Spurious wake (no other producer at v1.0 — defensive only). Re-arm.
    }

    unsigned int observed = __atomic_load_n(&shared_counter, __ATOMIC_ACQUIRE);
    if (observed != COUNTER_TARGET) {
        t_putstr("thread-probe: worker did not write the counter\n");
        t_exits(1);
    }

    t_putstr("thread-probe: ok (spawn + tid_address + exit + join verified)\n");
    t_exits(0);
}
