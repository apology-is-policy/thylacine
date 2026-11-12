// /pouch-hello-susp — the job-control vehicle: a POUCH (musl) program that can
// be stopped and resumed in a pts foreground group.
//
// Exists because the interactive suite had no leg that puts a POUCH program in
// a pts foreground process group and ^Zs it. pty-4.exp already drives the whole
// ladder (login -> ptyhost -> inner ut takes job control -> ^Z -> Stopped -> fg
// -> re-stop -> bg -> ^C), but its subject is `debug-child fgtick`, a NATIVE
// libthyla-rs program -- so no program whose dispositions the kernel reads from
// the Pouch note table is on the line. This program is that subject and nothing
// else. It installs no handler: the stop it takes is the kernel's default,
// applied when the key is posted.
//
// Output contract (the .exp matches on these, so they are ABI for the scenario):
//   SUSP-READY
//   SUSP-TICK <n> held     one line every 250 ms for as long as the program
//   SUSP-TICK <n> away     runs, n strictly increasing from 1: `held` while it
//                          holds the terminal, `away` while it does not
//   SUSP-NOTTY             fd 0 is no terminal of this session; exit status 2
//
// A LINE SAYS THE PROGRAM RUNS; `held` SAYS THE TERMINAL IS OURS. The shell
// spawns a job, puts it in a group and then hands it the terminal, and a ^Z
// typed before the last of those reaches the shell: the program runs on and
// the scenario waits for a stop that never comes. So a line says `held` only
// while this program leads a process group of its own and that group is the
// terminal's foreground group, and the scenario types its stop key after
// reading one. The group must be its own because it starts in the shell's,
// which holds the terminal: "my group is the foreground group" is true before
// the handoff too, and false in the middle of it. Run it alone, never in a
// pipeline.
//
// The line is written whoever holds the terminal, because the shell takes the
// terminal back as soon as it hears of a stop. A program that wrote only while
// it held the terminal would fall silent then whether or not it was ever
// parked, and its silence would say nothing about the stop.
//
// THE COUNTER IS THE POINT, not decoration. Proving "^Z stopped it" needs a
// NEGATIVE assertion (no output while stopped), and a negative is satisfied by
// a fixture that broke for any other reason -- a crash, a failed spawn, a shell
// that never put it in the foreground. So the scenario brackets the negative
// with two positives: lines ADVANCE before the stop, are ABSENT during it, and
// ADVANCE AGAIN after fg. The number is what makes the third leg real: a replay
// of buffered pre-stop output would show OLD numbers, so the post-resume leg
// demands one strictly greater than the last seen before ^Z. Without the
// counter that leg passes on stale bytes.
//
// Two constraints inherited from pty-4.exp's design, which paid for them:
//
//   NO TERMINAL READ. A v1.0 pts has no foreground-read arbitration (TTIN), so
//   a stopped job holding an outstanding terminal read steals the shell's
//   input. This program only ever WRITES, and asks who holds the terminal
//   (tcgetpgrp), which reads no input.
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
#include <unistd.h>

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
        pid_t fg = tcgetpgrp(STDIN_FILENO);
        if (fg < 0) {
            printf("SUSP-NOTTY\n");
            return 2;
        }
        pid_t own = getpgrp();
        printf("SUSP-TICK %lu %s\n", n, own == getpid() && fg == own ? "held" : "away");
        tick_sleep();
    }
}
