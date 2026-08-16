// /pouch-hello-reentry — proves the N-3 handler re-entrancy guard SUPPRESSES
// delivery, from userspace, deterministically.
//
// The gap: nothing tested that `in_handler` actually suppresses anything. The
// severity argument for the exec-drops-the-latch finding rested on READING the
// gate at notes.c:1244, not on exercising it. And it cannot be unit-tested --
// notes_deliver_at_el0_return takes no Thread argument (it reads
// current_thread()), so the EL0 tail is unreachable from the kernel suite. The
// two halves of N-3 both live in that tail, and one of them is not a condition
// at all but an ORDERING (the kill arm sits ABOVE the gate), which no C
// assertion can observe. So the honest test is in-guest, and this is it.
//
// THE MECHANISM, and why it is deterministic rather than a timing window:
//
//   sigaction(SIGINT)  -> the "interrupt" note   (catchable)
//   sigaction(SIGCHLD) -> the "child_exit" note  (catchable, distinct note)
//   raise(SIGINT)      -> the tail dispatches to handler_va and sets
//                         t->in_handler = true   (notes.c:1426)
//   inside that handler: raise(SIGCHLD)
//
// SIGCHLD is chosen over SIGPIPE deliberately: pouch MASKS SIGPIPE at startup,
// so a pipe-based second signal would be suppressed by the mask rather than by
// N-3, and the test would pass for the wrong reason.
//
// THE POINT THAT MAKES THE NEGATIVE REAL: a sleep inside the first handler.
// "The second note was not delivered" is worthless if the kernel never got the
// chance -- with no syscall inside the handler there may be no EL0 return at
// all, so the tail never runs and the gate is never consulted. Every nanosleep
// is a syscall, hence an EL0 return, hence the tail RUNNING and DECLINING to
// deliver. Without this the test asserts "nothing happened" against a path that
// was never taken, which is the vacuous shape this project keeps being bitten
// by.
//
// Output contract -- the assertion is ORDER, and the harness checks exactly it:
//
//   N3-ENTER          first handler entered
//   N3-STILL-IN       still inside it, AFTER several EL0 returns
//   N3-LEAVE          first handler about to return (SYS_NOTED)
//   N3-SECOND         second handler ran
//   N3-DONE           main resumed
//
// PASS  = that order.
// FAIL  = N3-SECOND appearing anywhere before N3-LEAVE. That is the gate
//         letting a note through while a handler runs.
// FAIL  = N3-SECOND missing entirely -- suppression must be a DEFERRAL, not a
//         drop. A guard that loses the note is a different bug with the same
//         happy-looking prefix, which is why N3-SECOND is asserted positively
//         and not merely asserted-absent-then-forgotten.

#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

static volatile sig_atomic_t in_first;
static volatile sig_atomic_t second_ran_inside;
static volatile sig_atomic_t second_ran;

static void nap(long ms) {
    struct timespec req = { .tv_sec = 0, .tv_nsec = ms * 1000 * 1000L };
    struct timespec rem;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) return;
        req = rem;
    }
}

static void handler_second(int sig) {
    (void)sig;
    // If this observes the first handler still active, N-3 did not hold. Record
    // it in a flag as well as printing: the ordering assertion is the harness's
    // job, but a program that can detect its own violation should say so
    // outright rather than leave it to a log reader.
    if (in_first) second_ran_inside = 1;
    second_ran = 1;
    printf("N3-SECOND\n");
}

static void handler_first(int sig) {
    (void)sig;
    in_first = 1;
    printf("N3-ENTER\n");

    // Post the second note from INSIDE the first handler.
    raise(SIGCHLD);

    // Several syscalls => several EL0 returns => the delivery tail runs and
    // must decline each time. This is the whole experiment.
    for (int i = 0; i < 5; i++) nap(20);

    printf("N3-STILL-IN\n");
    in_first = 0;
    printf("N3-LEAVE\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa;
    sa.sa_handler = handler_first;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        printf("N3-FAIL: sigaction(SIGINT)\n");
        return 1;
    }

    sa.sa_handler = handler_second;
    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        printf("N3-FAIL: sigaction(SIGCHLD)\n");
        return 1;
    }

    raise(SIGINT);

    // Give a deferred second note room to land after the first handler
    // returned. If it never arrives, suppression dropped it rather than
    // deferring it.
    for (int i = 0; i < 25 && !second_ran; i++) nap(20);

    printf("N3-DONE\n");

    if (second_ran_inside) {
        printf("N3-FAIL: second handler ran while the first was active\n");
        return 1;
    }
    if (!second_ran) {
        printf("N3-FAIL: second note never delivered -- suppressed permanently, not deferred\n");
        return 1;
    }
    printf("N3-PASS\n");
    return 0;
}
