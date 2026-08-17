// /pouch-hello-susp — the job-control vehicle: a POUCH (musl) program that can
// be stopped and resumed in a pts foreground group.
//
// Exists because the interactive suite had no leg that puts a POUCH program in
// a pts foreground process group and ^Zs it. pty-4.exp already drives the whole
// ladder (login -> ptyhost -> inner ut takes job control -> ^Z -> Stopped -> fg
// -> re-stop -> bg -> ^C), but its subject is `sleep`, a NATIVE libthyla-rs
// coreutil -- so the pouch boundary-line's note/disposition path is never on
// the line. This program is that subject and nothing else.
//
// Output contract (the .exp matches on these, so they are ABI for the scenario):
//   SUSP-READY
//   SUSP-TICK <n>          n strictly increasing from 1, forever
//
// THE COUNTER IS THE POINT, not decoration. Proving "^Z stopped it" needs a
// NEGATIVE assertion (no output while stopped), and a negative is satisfied by
// a fixture that broke for any other reason -- a crash, a failed spawn, a shell
// that never put it in the foreground. So the scenario brackets the negative
// with two positives: ticks ADVANCE before the stop, are ABSENT during it, and
// ADVANCE AGAIN after fg. The number is what makes the third leg real: a replay
// of buffered pre-stop output would show OLD numbers, so the post-resume leg
// demands one strictly greater than the last seen before ^Z. Without the
// counter that leg passes on stale bytes.
//
// Two constraints inherited from pty-4.exp's design, which paid for them:
//
//   NO TERMINAL READ. A v1.0 pts has no foreground-read arbitration (TTIN), so
//   a stopped job holding an outstanding terminal read steals the shell's
//   input. This program only ever WRITES.
//
//   NEVER EXITS ON ITS OWN. A job with its own deadline races the stop window
//   from both sides -- wall clock keeps running while stopped, which is the
//   artifact that originally masked PTY-4e F2's re-stop leg. This runs until
//   killed.
//
// The sleep is a RE-ARMING loop rather than sleep(3). SIGCONT interrupts the
// nanosleep, and whether musl restarts it or returns EINTR with the remainder
// is a detail this must not depend on: retrying on EINTR is correct under
// either, so the vehicle cannot introduce a phantom that looks like a kernel
// resume bug.

#include <errno.h>
#include <stdio.h>
#include <time.h>

// Slow enough that a human-scale expect window sees distinct ticks, fast enough
// that the scenario is not dominated by waiting.
#define TICK_NS (250 * 1000 * 1000L)

static void tick_sleep(void) {
    struct timespec req = { .tv_sec = 0, .tv_nsec = TICK_NS };
    struct timespec rem;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) return;
        req = rem;
    }
}

int main(void) {
    // Unbuffered: a stopped process must produce NO bytes, and stdio holding a
    // full buffer that flushes on resume would forge exactly the output the
    // scenario reads as "it never stopped".
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("SUSP-READY\n");

    for (unsigned long n = 1;; n++) {
        printf("SUSP-TICK %lu\n", n);
        tick_sleep();
    }
}
