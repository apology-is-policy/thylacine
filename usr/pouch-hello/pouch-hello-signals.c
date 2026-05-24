// /pouch-hello-signals — the POSIX signal proving binary (P6-pouch-signals,
// sub-chunk 13b). Exercises the boundary-line patch 0007 end-to-end:
//
//   sigaction(SIGINT, &handler, NULL)  -> __pouch_sigtab[SIGINT].sa_handler
//                                         is set; the kernel mask is unchanged
//                                         (NOTE_BIT_INTERRUPT not set).
//   raise(SIGINT)                      -> syscall(SYS_postnote, 0, "interrupt",
//                                         9). Kernel sees pid=0 self-post
//                                         sentinel; queues "interrupt" note;
//                                         at the EL0-return-tail of SYS_POSTNOTE
//                                         pops the note + dispatches via
//                                         __pouch_note_handler -> our user
//                                         handler. The handler sets a flag and
//                                         returns; SYS_NOTED(NCONT) restores
//                                         the saved user context; raise()
//                                         returns 0 with the flag set.
//   sigaction(SIGINT, SIG_IGN, NULL)   -> handler cleared.
//   raise(SIGINT)                      -> same wire shape but the bootstrap
//                                         dispatcher hits SIG_IGN -> NCONT
//                                         without invoking the user handler;
//                                         the flag must NOT change.
//   sigaction(SIGUSR1, ...)            -> EINVAL (unsupported v1.0 signum).
//
// Output (joey relays via the pipe-to-UART):
//   pouch-hello-signals: install handler
//   pouch-hello-signals: raise SIGINT
//   pouch-hello-signals: handler ran (count=1)
//   pouch-hello-signals: install SIG_IGN
//   pouch-hello-signals: raise SIGINT (ignored)
//   pouch-hello-signals: count unchanged (count=1)
//   pouch-hello-signals: unsupported sigaction returns EINVAL
//   pouch-hello-signals: exit 0
//
// Returns non-zero on any deviation — joey's content-check catches missing
// "exit 0" or non-zero exit status.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static volatile sig_atomic_t g_handler_count = 0;
static volatile int g_handler_sig = -1;

static void on_sigint(int sig) {
    g_handler_count++;
    g_handler_sig = sig;
}

int main(void) {
    printf("pouch-hello-signals: install handler\n");
    fflush(stdout);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        printf("pouch-hello-signals: sigaction(SIGINT) failed: errno=%d\n",
               errno);
        fflush(stdout);
        return 1;
    }

    printf("pouch-hello-signals: raise SIGINT\n");
    fflush(stdout);
    if (raise(SIGINT) != 0) {
        printf("pouch-hello-signals: raise(SIGINT) failed: errno=%d\n",
               errno);
        fflush(stdout);
        return 2;
    }

    // After raise() returns, the kernel has delivered the "interrupt" note
    // via the EL0-return-tail dispatcher; our handler has run and bumped
    // g_handler_count. The handler invocation is SYNCHRONOUS with raise()
    // — by the time raise() returns 0, the handler has executed (the note
    // delivery happened at the eret edge of SYS_POSTNOTE itself).
    if (g_handler_count != 1) {
        printf("pouch-hello-signals: handler did not run (count=%d, expected 1)\n",
               (int)g_handler_count);
        fflush(stdout);
        return 3;
    }
    if (g_handler_sig != SIGINT) {
        printf("pouch-hello-signals: handler got wrong sig (got=%d, expected %d)\n",
               g_handler_sig, SIGINT);
        fflush(stdout);
        return 4;
    }
    printf("pouch-hello-signals: handler ran (count=%d)\n",
           (int)g_handler_count);
    fflush(stdout);

    // ===== SIG_IGN path =====
    printf("pouch-hello-signals: install SIG_IGN\n");
    fflush(stdout);
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        printf("pouch-hello-signals: sigaction(SIGINT, SIG_IGN) failed\n");
        fflush(stdout);
        return 5;
    }

    printf("pouch-hello-signals: raise SIGINT (ignored)\n");
    fflush(stdout);
    if (raise(SIGINT) != 0) {
        printf("pouch-hello-signals: raise(SIGINT/IGN) failed: errno=%d\n",
               errno);
        fflush(stdout);
        return 6;
    }
    // The bootstrap dispatcher sees SIG_IGN and calls SYS_NOTED(NCONT)
    // without invoking the user handler; g_handler_count must stay at 1.
    if (g_handler_count != 1) {
        printf("pouch-hello-signals: SIG_IGN failed to suppress (count=%d, expected 1)\n",
               (int)g_handler_count);
        fflush(stdout);
        return 7;
    }
    printf("pouch-hello-signals: count unchanged (count=%d)\n",
           (int)g_handler_count);
    fflush(stdout);

    // ===== Unsupported signum path =====
    sa.sa_handler = on_sigint;
    int rc = sigaction(SIGUSR1, &sa, NULL);
    if (rc == 0) {
        printf("pouch-hello-signals: sigaction(SIGUSR1) unexpectedly succeeded\n");
        fflush(stdout);
        return 8;
    }
    if (errno != EINVAL) {
        printf("pouch-hello-signals: sigaction(SIGUSR1) wrong errno=%d (expected EINVAL=%d)\n",
               errno, EINVAL);
        fflush(stdout);
        return 9;
    }
    printf("pouch-hello-signals: unsupported sigaction returns EINVAL\n");
    fflush(stdout);

    printf("pouch-hello-signals: exit 0\n");
    fflush(stdout);
    return 0;
}
