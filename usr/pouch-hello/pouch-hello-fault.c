// /pouch-hello-fault — durable runtime regression for P6 hardening #3a
// (scripture e45a571 -- docs/ERRORS.md "snare:* family").
//
// Deliberately NULL-derefs to trigger an EL0 unhandled fault. The kernel
// MUST NOT extinct -- it must route the fault through
// arch_fault_handle's FAULT_UNHANDLED_USER branch + proc_fault_terminate
// (kernel/proc.c) to exits(NOTE_NAME_SNARE_SEGV). Parent joey's
// t_wait_pid then observes exit_status = 1 (v1.0 collapse; v1.x richer
// exit_status distinguishes fault-terminated from clean-non-zero).
//
// Pre-#3a (commit 26e3156 and earlier): the same NULL deref would
// produce "EXTINCTION: EL0 fault: no VMA covers vaddr / permission
// denied" and halt the kernel. Boot would not reach `Thylacine boot OK`.
//
// The marker "pouch-hello-fault: about to fault" must appear in joey's
// pipe-drain so the test can verify the binary actually ran (not just
// failed to start). The NULL deref happens IMMEDIATELY after the write,
// so any kernel-side regression that lets the fault path silently drop
// the Proc would be caught by the marker presence + non-zero exit.

#include <unistd.h>

int main(void) {
    static const char marker[] = "pouch-hello-fault: about to fault\n";
    (void)write(1, marker, sizeof(marker) - 1);

    // Take the fault. `volatile` defeats compiler dead-store
    // elimination + null-pointer-arithmetic UB optimization. The load
    // at vaddr 0 has no VMA covering it -> EL0 data abort ->
    // arch_fault_handle returns FAULT_UNHANDLED_USER ->
    // exception_sync_lower_el routes to
    // proc_fault_terminate(NOTE_NAME_SNARE_SEGV, 0).
    volatile int *bad = (volatile int *)0;
    int v = *bad;

    // UNREACHABLE -- the fault terminates this Proc before we get
    // here. The return statement exists to silence -Wreturn-type and
    // to prove to the optimizer that we'd use `v` if we reached it.
    return v;
}
