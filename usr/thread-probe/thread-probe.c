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

// --- #112 (CLONE_PARENT_SETTID) coverage --------------------------------
//
// The libt t_thread_spawn wrapper pins x4 = 0 (no publish), so a RAW 5-arg
// SVC is needed to drive the ptid arg. This is the only in-tree test that
// isolates the kernel publish from #111's child-side self-set: the pouch
// E2E (pouch-hello-threads) keeps the #111 `start()` self-set, which MASKS
// a broken kernel publish (the child sets its own tid regardless). Here,
// `publish_worker` never touches `publish_word`, so ONLY the kernel's
// pre-ready() store can change it from the sentinel.
static long thread_spawn_raw5(void *entry, void *sp_top, void *arg,
                              void *tls, void *ptid) {
    register long x0 __asm__("x0") = (long)(unsigned long)entry;
    register long x1 __asm__("x1") = (long)(unsigned long)sp_top;
    register long x2 __asm__("x2") = (long)(unsigned long)arg;
    register long x3 __asm__("x3") = (long)(unsigned long)tls;
    register long x4 __asm__("x4") = (long)(unsigned long)ptid;
    register long x8 __asm__("x8") = T_SYS_THREAD_SPAWN;
    __asm__ volatile ("svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc");
    return x0;
}

static unsigned char publish_stack[WORKER_STACK_SIZE]
    __attribute__((aligned(16)));
static volatile unsigned int publish_word;   // #112 ptid target (kernel-written)
static volatile unsigned int publish_join;   // clear_child_tid join word

__attribute__((noreturn))
static void publish_worker(void *arg) {
    (void)arg;                             // never touches publish_word
    (void)t_set_tid_address((void *)&publish_join);
    t_thread_exit();
}

int main(void) {
    __atomic_store_n(&worker_tidptr,   TIDPTR_SENTINEL, __ATOMIC_RELEASE);
    __atomic_store_n(&shared_counter,  0u,              __ATOMIC_RELEASE);

    wa.tidptr_va  = &worker_tidptr;
    wa.counter_va = &shared_counter;

    // --- #112 gate: a misaligned / out-of-bound ptid is rejected -EINVAL
    // BEFORE any Thread is created. The alignment gate is load-bearing --
    // without it the kernel would issue an UNaligned uaccess STR, which the
    // EL1 fixup table does NOT catch -> kernel extinction. (-EINVAL ==
    // -T_E_INVAL == -22.) These spawns are rejected, so they create no
    // Thread and leave publish_stack untouched.
    void *gsp = (void *)(publish_stack + WORKER_STACK_SIZE);
    long grc = thread_spawn_raw5((void *)publish_worker, gsp, (void *)0,
                                 (void *)0, (void *)0x100001ul);  // & 3 != 0
    if (grc != -22) {
        t_putstr("thread-probe: misaligned ptid not rejected -EINVAL (#112 gate)\n");
        t_exits(1);
    }
    grc = thread_spawn_raw5((void *)publish_worker, gsp, (void *)0,
                            (void *)0, (void *)0x800000000000ul); // >= 2^47
    if (grc != -22) {
        t_putstr("thread-probe: out-of-bound ptid not rejected -EINVAL (#112 gate)\n");
        t_exits(1);
    }

    // --- #112 publish: the kernel writes the new tid into *ptid BEFORE
    // ready(), so it is already set by the time the SVC returns -- regardless
    // of whether the child has run. publish_worker never touches publish_word.
    __atomic_store_n(&publish_word, 0xFFFFFFFFu,    __ATOMIC_RELEASE);
    __atomic_store_n(&publish_join, TIDPTR_SENTINEL, __ATOMIC_RELEASE);
    void *psp = (void *)(publish_stack + WORKER_STACK_SIZE);
    long ptid_tid = thread_spawn_raw5((void *)publish_worker, psp, (void *)0,
                                      (void *)0, (void *)&publish_word);
    if (ptid_tid < 0) {
        t_putstr("thread-probe: #112 ptid spawn failed\n");
        t_exits(1);
    }
    if (__atomic_load_n(&publish_word, __ATOMIC_ACQUIRE) != (unsigned int)ptid_tid) {
        t_putstr("thread-probe: kernel did NOT publish tid via ptid (#112)\n");
        t_exits(1);
    }
    // Reap the publish worker via its clear_child_tid join word.
    for (;;) {
        long rc = t_torpor_wait((unsigned int *)&publish_join, TIDPTR_SENTINEL, -1);
        if (rc != T_TORPOR_OK) {
            t_putstr("thread-probe: #112 publish-worker join failed\n");
            t_exits(1);
        }
        if (__atomic_load_n(&publish_join, __ATOMIC_ACQUIRE) == 0u) break;
    }

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

    t_putstr("thread-probe: ok (spawn + tid_address + exit + join + "
             "#112 ptid publish/gate verified)\n");
    t_exits(0);
}
