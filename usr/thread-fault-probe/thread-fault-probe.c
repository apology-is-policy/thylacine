// /thread-fault-probe -- HOLOTYPE RW-1 C-F1 regression. An EL0 fault in a
// MULTI-thread Proc must terminate the whole Proc (via the #809/#811 group
// cascade), NOT extinct the kernel -- the snare:* per-Proc-termination
// contract for exactly the stratumd-class (multi-thread) Proc.
//
// main spawns a peer Thread that blocks forever, so thread_count == 2 at
// fault time, then deliberately faults at EL0 (a store to an unmapped low
// VA). Pre-fix (proc_fault_terminate's stale thread_count>1 branch) the
// kernel EXTINCTED here and the boot never reached `Thylacine boot OK`.
// Post-fix the fault routes proc_fault_terminate -> exits("snare:segv") ->
// proc_group_terminate (wakes + kills the blocked worker via
// death-interruptible sleep) -> thread_exit_self, and joey reaps the Proc
// with a non-zero (fault-collapsed) status while the kernel stays up.
//
// Native libt (no pouch), mirroring /thread-probe + /pouch-hello-fault.

#include <thyla/syscall.h>

#define WORKER_STACK_SIZE 4096

static unsigned char worker_stack[WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

// The worker blocks on an address never written. #811 universal
// death-interruptible sleep lets the group cascade wake it (*_INTR); it
// then dies at its EL0-return die-check before re-sleeping. The for(;;)
// only guards a spurious wake.
static volatile unsigned int never_signalled;

__attribute__((noreturn))
static void worker_entry(void *arg) {
    (void)arg;
    __atomic_store_n(&never_signalled, 0xABCu, __ATOMIC_RELEASE);
    for (;;) {
        (void)t_torpor_wait((unsigned int *)&never_signalled, 0xABCu, -1);
    }
}

int main(void) {
    void *sp_top = (void *)(worker_stack + WORKER_STACK_SIZE);
    long tid = t_thread_spawn(worker_entry, sp_top, (void *)0, (void *)0);
    if (tid < 0) {
        t_putstr("thread-fault-probe: t_thread_spawn failed\n");
        t_exits(1);
    }

    // thread_count == 2 now (spawn links the peer synchronously). Fault.
    // A real store to an unmapped low VA -> data abort -> snare:segv (a
    // non-zero address so the compiler emits a store, not a __builtin_trap).
    t_putstr("thread-fault-probe: about to fault in a multi-thread Proc\n");
    volatile unsigned int *wild = (volatile unsigned int *)0x10;
    *wild = 0xdeadu;

    // UNREACHABLE -- the fault terminates the whole Proc.
    t_putstr("thread-fault-probe: SURVIVED THE FAULT -- BUG\n");
    t_exits(2);
}
