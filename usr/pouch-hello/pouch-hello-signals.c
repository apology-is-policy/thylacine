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
//   pouch-hello-signals: #96 FP/SIMD preserved across handler (V0-V31)
//   pouch-hello-signals: install SIG_IGN
//   pouch-hello-signals: raise SIGINT (ignored)
//   pouch-hello-signals: count unchanged (count=2)
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
    // Task #96: be a handler that touches FP/SIMD -- the realistic case, since
    // any float arithmetic or autovectorised memcpy does. Written as explicit
    // asm so the clobber is guaranteed rather than left to the compiler's
    // discretion; a handler that happened NOT to touch V regs would make the
    // preservation check below pass vacuously.
    __asm__ __volatile__(
        "movi v0.16b,  #0x11\n"  "movi v1.16b,  #0x11\n"
        "movi v2.16b,  #0x11\n"  "movi v3.16b,  #0x11\n"
        "movi v4.16b,  #0x11\n"  "movi v5.16b,  #0x11\n"
        "movi v6.16b,  #0x11\n"  "movi v7.16b,  #0x11\n"
        "movi v8.16b,  #0x11\n"  "movi v9.16b,  #0x11\n"
        "movi v10.16b, #0x11\n"  "movi v11.16b, #0x11\n"
        "movi v12.16b, #0x11\n"  "movi v13.16b, #0x11\n"
        "movi v14.16b, #0x11\n"  "movi v15.16b, #0x11\n"
        "movi v16.16b, #0x11\n"  "movi v17.16b, #0x11\n"
        "movi v18.16b, #0x11\n"  "movi v19.16b, #0x11\n"
        "movi v20.16b, #0x11\n"  "movi v21.16b, #0x11\n"
        "movi v22.16b, #0x11\n"  "movi v23.16b, #0x11\n"
        "movi v24.16b, #0x11\n"  "movi v25.16b, #0x11\n"
        "movi v26.16b, #0x11\n"  "movi v27.16b, #0x11\n"
        "movi v28.16b, #0x11\n"  "movi v29.16b, #0x11\n"
        "movi v30.16b, #0x11\n"  "movi v31.16b, #0x11\n"
        : :
        : "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31");
}

// Task #96 sentinel buffers. 16-byte aligned for STP/LDP-Q.
static unsigned char g_fp_sent[512] __attribute__((aligned(16)));
static unsigned char g_fp_seen[512] __attribute__((aligned(16)));

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

    // ===== Task #96: FP/SIMD survives a note handler =====
    //
    // A handler runs on the SAME thread with no context switch, so
    // cpu_switch_context's eager FP save does not apply; without an explicit
    // save/restore in notes.c the handler's V registers leak back into the
    // interrupted computation. This is the NATIVE (Plan 9) delivery path --
    // pouch signals go out via SYS_NOTIFY and come back through
    // notes_deliver_at_el0_return, so it proves that save site specifically.
    //
    // The load / syscall / store must be ONE asm block. A C-level raise() is
    // an ordinary call, and AAPCS64 lets a call clobber V0-V7 and V16-V31 --
    // so the compiler would be entitled to exactly the corruption being
    // tested for, and a C-level check could not tell the two apart. Inside
    // one block the svc is the only thing that runs, and the handler is
    // dispatched at its EL0-return tail: a genuine asynchronous interruption.
    for (int k = 0; k < 32; k++)
        for (int j = 0; j < 16; j++)
            g_fp_sent[k * 16 + j] = (unsigned char)(0x40 + k);

    const char *note = "interrupt";
    long praise = 0;
    __asm__ __volatile__(
        "ldp q0,  q1,  [%[s], #0]\n"   "ldp q2,  q3,  [%[s], #32]\n"
        "ldp q4,  q5,  [%[s], #64]\n"  "ldp q6,  q7,  [%[s], #96]\n"
        "ldp q8,  q9,  [%[s], #128]\n" "ldp q10, q11, [%[s], #160]\n"
        "ldp q12, q13, [%[s], #192]\n" "ldp q14, q15, [%[s], #224]\n"
        "ldp q16, q17, [%[s], #256]\n" "ldp q18, q19, [%[s], #288]\n"
        "ldp q20, q21, [%[s], #320]\n" "ldp q22, q23, [%[s], #352]\n"
        "ldp q24, q25, [%[s], #384]\n" "ldp q26, q27, [%[s], #416]\n"
        "ldp q28, q29, [%[s], #448]\n" "ldp q30, q31, [%[s], #480]\n"
        "mov x0, #0\n"              // pid 0 = self-post
        "mov x1, %[nm]\n"           // note name
        "mov x2, #9\n"              // strlen("interrupt")
        "mov x8, #47\n"             // SYS_POSTNOTE
        "svc #0\n"                  // handler is dispatched at this eret edge
        "mov %[r], x0\n"
        "stp q0,  q1,  [%[d], #0]\n"   "stp q2,  q3,  [%[d], #32]\n"
        "stp q4,  q5,  [%[d], #64]\n"  "stp q6,  q7,  [%[d], #96]\n"
        "stp q8,  q9,  [%[d], #128]\n" "stp q10, q11, [%[d], #160]\n"
        "stp q12, q13, [%[d], #192]\n" "stp q14, q15, [%[d], #224]\n"
        "stp q16, q17, [%[d], #256]\n" "stp q18, q19, [%[d], #288]\n"
        "stp q20, q21, [%[d], #320]\n" "stp q22, q23, [%[d], #352]\n"
        "stp q24, q25, [%[d], #384]\n" "stp q26, q27, [%[d], #416]\n"
        "stp q28, q29, [%[d], #448]\n" "stp q30, q31, [%[d], #480]\n"
        : [r] "=r" (praise)
        : [s] "r" (g_fp_sent), [d] "r" (g_fp_seen), [nm] "r" (note)
        : "x0","x1","x2","x8","memory",
          "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31");

    if (praise != 0) {
        printf("pouch-hello-signals: #96 self-post failed (ret=%ld)\n", praise);
        fflush(stdout);
        return 10;
    }
    // The handler must have run, or the V registers were never at risk and
    // the comparison below would pass on a completely unfixed kernel.
    if (g_handler_count != 2) {
        printf("pouch-hello-signals: #96 handler did not run (count=%d, expected 2)\n",
               (int)g_handler_count);
        fflush(stdout);
        return 11;
    }
    for (int i = 0; i < 512; i++) {
        if (g_fp_seen[i] != g_fp_sent[i]) {
            printf("pouch-hello-signals: #96 FP CLOBBERED across handler "
                   "(V%d byte %d: got 0x%02x want 0x%02x)\n",
                   i / 16, i % 16, g_fp_seen[i], g_fp_sent[i]);
            fflush(stdout);
            return 12;
        }
    }
    printf("pouch-hello-signals: #96 FP/SIMD preserved across handler (V0-V31)\n");
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
    // without invoking the user handler; g_handler_count must stay at 2
    // (one from the raise above, one from the #96 FP leg's self-post).
    if (g_handler_count != 2) {
        printf("pouch-hello-signals: SIG_IGN failed to suppress (count=%d, expected 2)\n",
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
