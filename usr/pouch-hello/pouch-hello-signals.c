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
//   sigaction(SIGINT, SIG_IGN, NULL)   -> the kernel's row ignores it.
//   raise(SIGINT)                      -> same wire shape but the post drops the
//                                         note, so the user handler does not
//                                         run; the flag must NOT change.
//   sigaction(SIGALRM, ...)            -> EINVAL (unsupported v1.0 signum).
//   deaths (gcpark-b)                  -> the binary re-spawns itself to die
//                                         by abort(), a failed assert(), a
//                                         plain `brk #1`, a NULL store,
//                                         raise(SIGABRT) and kill(getpid(),
//                                         SIGABRT); each waitpid must read
//                                         WIFSIGNALED with SIGABRT, SIGABRT,
//                                         SIGTRAP, SIGSEGV, SIGABRT, SIGABRT --
//                                         never WIFEXITED (docs/ERRORS.md
//                                         "The wait status").
//   self faults (signal chunk S-3)     -> children that raise(SIGSEGV),
//                                         kill(getpid(), SIGBUS), raise(SIGILL)
//                                         and kill(0, SIGFPE) read WIFSIGNALED
//                                         with that signal. Before patch 0050
//                                         each answered EINVAL and ran on.
//   errnos (signal chunk S-3)          -> kill() of a missing pid reads ESRCH,
//                                         and a child's kill() of its parent
//                                         reads EPERM; both read EIO before
//                                         SYS_POSTNOTE answered errnos.
//   SIGTERM apart (signal chunk S-2)   -> with SIGINT ignored, raise(SIGINT)
//                                         must not run the SIGTERM handler, and
//                                         with SIGINT blocked raise(SIGTERM) must
//                                         run it at once; a child killed with
//                                         SIGTERM reads WIFSIGNALED SIGTERM.
//                                         Before patch 0049 both rode
//                                         `interrupt`, and all three failed.
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
//   pouch-hello-signals: SIGTERM runs its own handler, apart from SIGINT
//   pouch-hello-signals: SIGUSR1 and SIGUSR2 run their own handlers, each blocked apart
//   pouch-hello-signals: death abort reads WIFSIGNALED SIGABRT  (x4 legs)
//   pouch-hello-signals: death raise-segv reads WIFSIGNALED SIGSEGV  (x4 legs)
//   pouch-hello-signals: death term reads WIFSIGNALED SIGTERM
//   pouch-hello-signals: death usr1 reads WIFSIGNALED SIGUSR1  (and usr2)
//   pouch-hello-signals: kill errnos read ESRCH and EPERM
//   pouch-hello-signals: blocked <leg> ok  (x15 legs; see test_blocked)
//   pouch-hello-signals: a poll or select over no fds sleeps for its timeout
//   POUCH_CENSUS_SIGNALS (pouch-census.h; joey matches it)
//
// Returns non-zero on any deviation — joey's content-check catches missing
// "exit 0" or non-zero exit status.

#undef NDEBUG
#include "pouch-census.h"
#include <assert.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>

extern char **environ;

static const char SELF[] = "/bin/pouch-hello-signals";

// CHILD: die one way. Returns only if the death did not happen.
static int die(const char *how) {
    if (!strcmp(how, "abort"))  abort();
    if (!strcmp(how, "assert")) { volatile int zero = 0; assert(zero); }
    if (!strcmp(how, "brk"))    __asm__ __volatile__("brk #1" ::: "memory");
    if (!strcmp(how, "segv"))   { volatile int *bad = (volatile int *)0; *bad = 1; }
    if (!strcmp(how, "raise"))  raise(SIGABRT);
    if (!strcmp(how, "killself")) kill(getpid(), SIGABRT);
    if (!strcmp(how, "raise-segv")) raise(SIGSEGV);
    if (!strcmp(how, "kill-bus"))   kill(getpid(), SIGBUS);
    if (!strcmp(how, "raise-ill"))  raise(SIGILL);
    if (!strcmp(how, "kill0-fpe"))  kill(0, SIGFPE);
    if (!strcmp(how, "wait"))   for (;;) sleep(1);
    return 0;
}

// CHILD: kill() its parent, which is not its child, and exit 0 only if the
// refusal reads EPERM. SIGCHLD's note is one the parent ignores by default, so
// a broken gate would still leave the parent standing to report it.
static int kill_parent(const char *ppid_arg) {
    pid_t ppid = (pid_t)atoi(ppid_arg);
    errno = 0;
    int rc = kill(ppid, SIGCHLD);
    if (rc == -1 && errno == EPERM) return 0;
    printf("pouch-hello-signals: kill(parent) rc=%d errno=%d (want -1 EPERM=%d)\n",
           rc, errno, EPERM);
    return 3;
}

// Each death must reach the parent as the signal the kernel recorded for it.
// Before gcpark-b every one read WIFEXITED: abort() as 127, the faults as 1.
// Before patch 0049 raise(SIGABRT) and kill(getpid(), SIGABRT) answered
// EINVAL and the child ran on to exit 0; before patch 0050 so did the four
// fault signals sent to oneself.
static int test_deaths(void) {
    static const struct { const char *how; int sig; const char *name; } legs[] = {
        { "abort",  SIGABRT, "SIGABRT" },
        { "assert", SIGABRT, "SIGABRT" },
        { "brk",    SIGTRAP, "SIGTRAP" },
        { "segv",   SIGSEGV, "SIGSEGV" },
        { "raise",  SIGABRT, "SIGABRT" },
        { "killself", SIGABRT, "SIGABRT" },
        { "raise-segv", SIGSEGV, "SIGSEGV" },
        { "kill-bus",   SIGBUS,  "SIGBUS"  },
        { "raise-ill",  SIGILL,  "SIGILL"  },
        { "kill0-fpe",  SIGFPE,  "SIGFPE"  },
    };
    // Every leg runs and reports, so one regression cannot hide another.
    int bad = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        char *cargv[] = { (char *)SELF, (char *)"die", (char *)legs[i].how, NULL };
        pid_t pid;
        int st = 0;
        if (posix_spawn(&pid, SELF, NULL, NULL, cargv, environ) != 0) {
            printf("pouch-hello-signals: death %s: respawn failed errno=%d\n",
                   legs[i].how, errno);
            return -1;
        }
        if (waitpid(pid, &st, 0) != pid) {
            printf("pouch-hello-signals: death %s: waitpid failed errno=%d\n",
                   legs[i].how, errno);
            return -1;
        }
        if (!WIFSIGNALED(st) || WTERMSIG(st) != legs[i].sig) {
            printf("pouch-hello-signals: death %s: status %#x (want WIFSIGNALED %s)\n",
                   legs[i].how, (unsigned)st, legs[i].name);
            bad = 1;
            continue;
        }
        printf("pouch-hello-signals: death %s reads WIFSIGNALED %s\n",
               legs[i].how, legs[i].name);
    }
    return bad ? -1 : 0;
}

// kill(pid, sig) posts the signal's own note, and the child's death reads back as
// that signal. Before patch 0049 Pouch posted `interrupt` for SIGTERM and this read
// SIGINT. The child may not have reached main yet: a note it has no handler for ends
// it the same way, by the kernel's default.
static int test_kill_death(int sig, const char *note, const char *signame) {
    char *cargv[] = { (char *)SELF, (char *)"die", (char *)"wait", NULL };
    pid_t pid;
    int st = 0;
    if (posix_spawn(&pid, SELF, NULL, NULL, cargv, environ) != 0) {
        printf("pouch-hello-signals: death %s: respawn failed errno=%d\n", note, errno);
        return -1;
    }
    if (kill(pid, sig) != 0) {
        printf("pouch-hello-signals: death %s: kill failed errno=%d\n", note, errno);
        return -1;
    }
    if (waitpid(pid, &st, 0) != pid) {
        printf("pouch-hello-signals: death %s: waitpid failed errno=%d\n", note, errno);
        return -1;
    }
    if (!WIFSIGNALED(st) || WTERMSIG(st) != sig) {
        printf("pouch-hello-signals: death %s: status %#x (want WIFSIGNALED %s)\n",
               note, (unsigned)st, signame);
        return -1;
    }
    printf("pouch-hello-signals: death %s reads WIFSIGNALED %s\n", note, signame);
    return 0;
}

// kill() names its refusals the way POSIX does. The missing pid is far above the
// live pid counter. The not-a-child case runs in a child, because Pouch has no
// getppid: the child is handed this process's pid and kills it.
static int test_errnos(void) {
    int bad = 0;
    errno = 0;
    int rc = kill((pid_t)0x3ffffff0, SIGCHLD);
    if (rc != -1 || errno != ESRCH) {
        printf("pouch-hello-signals: kill(missing pid) rc=%d errno=%d (want -1 ESRCH=%d)\n",
               rc, errno, ESRCH);
        bad = 1;
    }

    char ppid[16];
    snprintf(ppid, sizeof ppid, "%d", (int)getpid());
    char *cargv[] = { (char *)SELF, (char *)"kill-parent", ppid, NULL };
    pid_t pid;
    int st = 0;
    if (posix_spawn(&pid, SELF, NULL, NULL, cargv, environ) != 0) {
        printf("pouch-hello-signals: kill-parent: respawn failed errno=%d\n", errno);
        return -1;
    }
    if (waitpid(pid, &st, 0) != pid) {
        printf("pouch-hello-signals: kill-parent: waitpid failed errno=%d\n", errno);
        return -1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("pouch-hello-signals: kill-parent: status %#x (want exit 0)\n", (unsigned)st);
        bad = 1;
    }
    if (bad) return -1;
    printf("pouch-hello-signals: kill errnos read ESRCH and EPERM\n");
    return 0;
}

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

// SIGINT is SIG_IGN when this runs. Until 0049 a delivered `interrupt` ran
// SIGTERM's handler whenever SIGINT had none, and blocking SIGINT blocked the
// note SIGTERM rode, so each of the two checks below failed.
static int test_term_apart(void) {
    struct sigaction ta;
    memset(&ta, 0, sizeof ta);
    ta.sa_handler = on_sigint;
    if (sigaction(SIGTERM, &ta, NULL) != 0) {
        printf("pouch-hello-signals: sigaction(SIGTERM) failed: errno=%d\n", errno);
        return -1;
    }
    int before = (int)g_handler_count;
    raise(SIGINT);
    int after_int = (int)g_handler_count;

    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT);
    sigprocmask(SIG_BLOCK, &blk, &old);
    g_handler_sig = -1;
    raise(SIGTERM);
    int after_term = (int)g_handler_count;
    int term_sig   = g_handler_sig;
    sigprocmask(SIG_SETMASK, &old, NULL);

    ta.sa_handler = SIG_DFL;
    sigaction(SIGTERM, &ta, NULL);

    if (after_int != before) {
        printf("pouch-hello-signals: raise(SIGINT) ran the SIGTERM handler (count %d -> %d)\n",
               before, after_int);
        return -1;
    }
    if (after_term != before + 1 || term_sig != SIGTERM) {
        printf("pouch-hello-signals: raise(SIGTERM) with SIGINT blocked: count %d -> %d, sig %d\n",
               before, after_term, term_sig);
        return -1;
    }
    printf("pouch-hello-signals: SIGTERM runs its own handler, apart from SIGINT\n");
    return 0;
}

// SIGUSR1 and SIGUSR2 have a note each (0068). A handler on one runs at once with its
// own number while the other is blocked, and the blocked one waits for its unblock:
// the family bit reaches the kernel's mask and comes back through the shadow.
static int test_usr_apart(void) {
    struct sigaction ua;
    memset(&ua, 0, sizeof ua);
    ua.sa_handler = on_sigint;
    if (sigaction(SIGUSR1, &ua, NULL) != 0 || sigaction(SIGUSR2, &ua, NULL) != 0) {
        printf("pouch-hello-signals: sigaction(SIGUSR1/2) failed: errno=%d\n", errno);
        return -1;
    }
    sigset_t blk, old, seen;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, &old);
    sigprocmask(SIG_BLOCK, NULL, &seen);
    int blocked = sigismember(&seen, SIGUSR2) == 1 && sigismember(&seen, SIGUSR1) != 1;

    int before = (int)g_handler_count;
    g_handler_sig = -1;
    raise(SIGUSR1);
    int after1 = (int)g_handler_count;
    int sig1   = g_handler_sig;
    raise(SIGUSR2);
    int after2 = (int)g_handler_count;
    sigprocmask(SIG_SETMASK, &old, NULL);
    int after3 = (int)g_handler_count;
    int sig3   = g_handler_sig;

    ua.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &ua, NULL);
    sigaction(SIGUSR2, &ua, NULL);

    if (!blocked || after1 != before + 1 || sig1 != SIGUSR1 || after2 != after1 ||
        after3 != after2 + 1 || sig3 != SIGUSR2) {
        printf("pouch-hello-signals: usr-apart: blocked=%d count %d -> %d -> %d -> %d, sig %d then %d\n",
               blocked, before, after1, after2, after3, sig1, sig3);
        return -1;
    }
    printf("pouch-hello-signals: SIGUSR1 and SIGUSR2 run their own handlers, each blocked apart\n");
    return 0;
}

// ===== A blocked call and a note (ARCHITECTURE 8.8.3) =====
//
// Each leg is a fresh image, so it shows what that image did with nothing left over
// from the legs above. The child holds two fixed fds: LEG_DATA, a pipe it blocks
// reading, and LEG_STATUS, where it writes 'R' just before it blocks and its handler
// writes 'H'. The driver watches /proc/<pid>/sched until the child's thread sleeps and
// only then posts, so a witness reaches the wait it names. A wait a note fails to end
// is a deadline in the driver, never a hung boot.
// Pouch's posix_spawn builds the child's table with no holes and carries only what a
// dup2 names, so these sit just above the standard streams, and nothing needs closing.
#define LEG_DATA         3
#define LEG_STATUS       4
#define LEG_DEADLINE_MS  20000LL
#define LEG_QUIET_MS     100

static volatile sig_atomic_t g_leg_caught;
static volatile pid_t g_leg_kid;

static long long leg_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static void leg_handler(int sig) {
    (void)sig;
    g_leg_caught++;
    char h = 'H';
    (void)write(LEG_STATUS, &h, 1);
}

// A launcher's shape: the handler ends the child the wait is for, and the wait,
// restarted, reaps it.
static void leg_handler_kills(int sig) {
    leg_handler(sig);
    kill(g_leg_kid, SIGTERM);
}

static int leg_catch(int sig, void (*h)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static void leg_ready(void) {
    char r = 'R';
    (void)write(LEG_STATUS, &r, 1);
}

static int leg_is_asleep(pid_t pid) {
    char path[32], buf[512];
    snprintf(path, sizeof path, "/proc/%d/sched", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, " SLP\n") != NULL;
}

// Two readings apart, because one can catch the child paging in the call it is about
// to make.
static int leg_wait_asleep(pid_t pid, long long deadline) {
    int seen = 0;
    while (leg_now_ms() < deadline) {
        seen = leg_is_asleep(pid) ? seen + 1 : 0;
        if (seen >= 2) return 0;
        usleep(5000);
    }
    return -1;
}

// Reap pid by the deadline. With repost, post again each second: a note that still
// beat the wait ran its handler early and left the wait standing, and a post that
// reaches the wait is what the leg is for. The kill on the way out keeps a failure
// from outliving its report.
static int leg_wait_exit(pid_t pid, int *st, int repost, long long deadline) {
    long long next = leg_now_ms() + 1000;
    for (;;) {
        pid_t r = waitpid(pid, st, WNOHANG);
        if (r == pid) return 0;
        if (r < 0 && errno != EINTR) return -1;
        long long now = leg_now_ms();
        if (now >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, st, 0);
            return -1;
        }
        if (repost && now >= next) {
            kill(pid, repost);
            next = now + 1000;
        }
        usleep(2000);
    }
}

static int leg_spawn_waiter(pid_t *out) {
    char *a[] = { (char *)SELF, (char *)"die", (char *)"wait", NULL };
    return posix_spawn(out, SELF, NULL, NULL, a, environ);
}

static jmp_buf g_leg_escape;

static void leg_handler_escapes(int sig) {
    (void)sig;
    longjmp(g_leg_escape, 1);
}

// Pouch declines fork (POUCH-DESIGN 8.3), and SYS_clone is its sentinel, so what crosses
// the kernel's fork is reached the way a native program reaches it: SYS_RFORK, RFPROC
// alone, on this stack and thread pointer. The child runs nothing that needs musl's
// thread list set up again: read, write, wait and _exit.
#define LEG_SYS_RFORK 105
#define LEG_RFPROC    1L

// Nor has Pouch an exec: SYS_execve is the same sentinel. SYS_EXECVE takes the path by
// length and argc packed strings, each with its NUL, and keeps the descriptors.
#define LEG_SYS_EXECVE 104

static pid_t leg_native_fork(void) {
    return (pid_t)syscall(LEG_SYS_RFORK, LEG_RFPROC, 0L, 0L);
}

// With SIGUSR1 blocked, reap 14 children and count how many notes then fit. The kernel
// discards a dead child's note at the tail, so all of NOTE_QUEUE_DEPTH's 16 slots are
// free. With no table governing, each child leaves an entry until they coalesce at 12.
static int leg_ring_room(void) {
    sigset_t s;
    int st = 0;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &s, NULL) != 0) return -1;
    for (int i = 0; i < 14; i++) {
        pid_t k = leg_native_fork();
        if (k < 0) return -1;
        if (k == 0) _exit(0);
        if (waitpid(k, &st, 0) != k) return -1;
    }
    int room = 0;
    while (room < 32 && kill(getpid(), SIGUSR1) == 0) room++;
    return room;
}

// CHILD: one leg. Exit 0 is the leg's own claim holding. A code above that names what
// it saw instead.
static int leg_child(const char *mode) {
    char b = 0;
    ssize_t n;

    if (!strcmp(mode, "dfl-reset")) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        mode = "dfl";
    }
    if (!strcmp(mode, "dfl-after-handler")) {
        if (leg_catch(SIGINT, leg_handler, 0) != 0) return 60;
        signal(SIGINT, SIG_DFL);
        mode = "dfl";
    }
    if (!strcmp(mode, "exec-dfl")) {
        if (leg_catch(SIGINT, leg_handler, 0) != 0) return 60;
        static const char args[] = "/bin/pouch-hello-signals\0leg\0dfl";
        syscall(LEG_SYS_EXECVE, SELF, sizeof SELF - 1, args, sizeof args, 3L);
        return 61;
    }
    if (!strcmp(mode, "dfl")) {
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return 62;
    }
    if (!strcmp(mode, "intr-norestart")) {
        if (leg_catch(SIGUSR1, leg_handler, 0) != 0) return 60;
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return n == -1 && errno == EINTR && g_leg_caught >= 1 ? 0 : 63;
    }
    if (!strcmp(mode, "intr-restart")) {
        if (leg_catch(SIGUSR1, leg_handler, SA_RESTART) != 0) return 60;
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return n == 1 && b == 'D' && g_leg_caught >= 1 ? 0 : 64;
    }
    if (!strcmp(mode, "signal-restart")) {
        if (signal(SIGUSR2, leg_handler) == SIG_ERR) return 60;
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return n == 1 && b == 'D' && g_leg_caught >= 1 ? 0 : 65;
    }
    if (!strcmp(mode, "quiet")) {
        if (leg_catch(SIGUSR1, leg_handler, 0) != 0) return 60;
        if (signal(SIGINT, SIG_IGN) == SIG_ERR) return 60;
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return n == 1 && b == 'D' && g_leg_caught == 0 ? 0 : 66;
    }
    if (!strcmp(mode, "pause")) {
        if (leg_catch(SIGUSR1, leg_handler, 0) != 0) return 60;
        leg_ready();
        int rc = pause();
        return rc == -1 && errno == EINTR && g_leg_caught >= 1 ? 0 : 67;
    }
    if (!strcmp(mode, "nanosleep")) {
        if (leg_catch(SIGUSR1, leg_handler, 0) != 0) return 60;
        leg_ready();
        struct timespec req = { 60, 0 }, rem = { 0, 0 };
        int rc = nanosleep(&req, &rem);
        return rc == -1 && errno == EINTR && g_leg_caught >= 1 &&
               rem.tv_sec > 0 && rem.tv_sec <= 60 ? 0 : 68;
    }
    if (!strcmp(mode, "wait-norestart")) {
        pid_t kid;
        int st = 0;
        if (leg_catch(SIGUSR1, leg_handler, 0) != 0) return 60;
        if (leg_spawn_waiter(&kid) != 0) return 60;
        leg_ready();
        int rc = waitpid(kid, &st, 0);
        int e = errno;
        kill(kid, SIGKILL);
        pid_t reaped = waitpid(kid, &st, 0);
        return rc == -1 && e == EINTR && g_leg_caught >= 1 && reaped == kid ? 0 : 69;
    }
    if (!strcmp(mode, "wait-restart")) {
        pid_t kid;
        int st = 0;
        if (leg_spawn_waiter(&kid) != 0) return 60;
        g_leg_kid = kid;
        if (leg_catch(SIGUSR1, leg_handler_kills, SA_RESTART) != 0) return 60;
        leg_ready();
        int rc = waitpid(kid, &st, 0);
        return rc == kid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM &&
               g_leg_caught >= 1 ? 0 : 70;
    }
    if (!strcmp(mode, "escape")) {
        if (leg_catch(SIGUSR1, leg_handler_escapes, 0) != 0) return 60;
        if (setjmp(g_leg_escape) == 0) {
            raise(SIGUSR1);
            return 76;
        }
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return 77;
    }
    if (!strcmp(mode, "ring-room")) {
        // A forked child first, whose table crossed the fork; the ring is then still
        // empty for the image's own count.
        int st = 0;
        pid_t kid = leg_native_fork();
        if (kid < 0) return 60;
        if (kid == 0) _exit(leg_ring_room());
        if (waitpid(kid, &st, 0) != kid) return 60;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 16) return 120 + (WEXITSTATUS(st) & 31);
        int room = leg_ring_room();
        if (room != 16) return 80 + (room & 31);
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return n == 1 && b == 'D' ? 0 : 78;
    }
    if (!strcmp(mode, "fork-read")) {
        int p[2];
        if (leg_catch(SIGUSR1, leg_handler, 0) != 0) return 60;
        if (pipe(p) != 0) return 60;
        pid_t kid = leg_native_fork();
        if (kid < 0) return 60;
        if (kid == 0) {
            close(p[0]);
            char r = 'R';
            (void)write(p[1], &r, 1);
            n = read(LEG_DATA, &b, 1);
            _exit(n == -1 && errno == EINTR && g_leg_caught >= 1 ? 0 : 71);
        }
        close(p[1]);
        char r = 0;
        if (read(p[0], &r, 1) != 1) return 72;
        long long deadline = leg_now_ms() + LEG_DEADLINE_MS;
        int st = 0;
        if (leg_wait_asleep(kid, deadline) != 0) {
            kill(kid, SIGKILL);
            waitpid(kid, &st, 0);
            return 73;
        }
        kill(kid, SIGUSR1);
        if (leg_wait_exit(kid, &st, SIGUSR1, deadline) != 0) return 74;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 75;
        leg_ready();
        n = read(LEG_DATA, &b, 1);
        return n == 1 && b == 'D' ? 0 : 78;
    }
    return 59;
}

// One byte from the status pipe equal to `want`, by the deadline. 'H' can precede
// nothing else that matters, so anything else read on the way is passed over.
static int leg_read_status(int fd, char want, long long deadline) {
    for (;;) {
        long long left = deadline - leg_now_ms();
        if (left <= 0) return -1;
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        char c = 0;
        if (read(fd, &c, 1) != 1) return -1;
        if (c == want) return 0;
    }
}

struct blocked_leg {
    const char *mode;
    int sig;     // the note the driver posts once the child sleeps; 0 for none
    int also;    // a second note the row must also keep from the wait
    int repost;  // keep posting until the child exits
    int ack;     // the handler must ack before any data goes
    int data;    // write the byte that ends the read
    int dies;    // the signal the child must die of; 0 means it must exit 0
};

static int run_blocked_leg(const struct blocked_leg *l) {
    int data[2], status[2];
    if (pipe(data) != 0 || pipe(status) != 0) {
        printf("pouch-hello-signals: blocked %s: pipe errno=%d\n", l->mode, errno);
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, data[0], LEG_DATA);
    posix_spawn_file_actions_adddup2(&fa, status[1], LEG_STATUS);
    char *cargv[] = { (char *)SELF, (char *)"leg", (char *)l->mode, NULL };
    pid_t pid;
    int sp = posix_spawn(&pid, SELF, &fa, NULL, cargv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(data[0]);
    close(status[1]);
    int rc = -1;
    int st = 0;
    if (sp != 0) {
        printf("pouch-hello-signals: blocked %s: spawn=%d\n", l->mode, sp);
        goto out;
    }

    long long deadline = leg_now_ms() + LEG_DEADLINE_MS;
    const char *why = NULL;
    if (leg_read_status(status[0], 'R', deadline) != 0) {
        why = "never reached its wait";
    } else if (leg_wait_asleep(pid, deadline) != 0) {
        why = "never slept";
    } else {
        if (l->sig) kill(pid, l->sig);
        if (l->also) kill(pid, l->also);
        if (l->ack && leg_read_status(status[0], 'H', deadline) != 0) {
            why = "handler did not run during the wait";
        } else {
            if (!l->ack && !l->repost && !l->dies) usleep(LEG_QUIET_MS * 1000);
            if (l->data) {
                char d = 'D';
                (void)write(data[1], &d, 1);
            }
        }
    }
    if (why) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        printf("pouch-hello-signals: blocked %s: %s (status %#x)\n", l->mode, why, (unsigned)st);
        goto out;
    }
    if (leg_wait_exit(pid, &st, l->repost ? l->sig : 0, deadline) != 0) {
        printf("pouch-hello-signals: blocked %s: alive at the deadline\n", l->mode);
        goto out;
    }
    int good = l->dies ? WIFSIGNALED(st) && WTERMSIG(st) == l->dies
                       : WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!good) {
        printf("pouch-hello-signals: blocked %s: status %#x (want %s %d)\n",
               l->mode, (unsigned)st, l->dies ? "signal" : "exit", l->dies);
        goto out;
    }
    printf("pouch-hello-signals: blocked %s ok\n", l->mode);
    rc = 0;
out:
    close(data[1]);
    close(status[0]);
    return rc;
}

// The positive and the negative halves are one variable apart: intr-norestart and
// quiet are the same child, and only the note's row differs. Every leg runs and
// reports, so one regression cannot hide another.
static int test_blocked(void) {
    static const struct blocked_leg legs[] = {
        // A program with no handler dies in its wait: nothing registered at startup.
        { .mode = "dfl",               .sig = SIGINT,  .dies = SIGINT  },
        { .mode = "dfl",               .sig = SIGTERM, .dies = SIGTERM },
        { .mode = "dfl",               .sig = SIGUSR1, .dies = SIGUSR1 },
        // Resetting a signal that never had a row dies the same.
        { .mode = "dfl-reset",         .sig = SIGINT,  .dies = SIGINT  },
        // A dispatcher is registered, and this note's row says default.
        { .mode = "dfl-after-handler", .sig = SIGINT,  .dies = SIGINT  },
        // Exec drops the table and the handler it held.
        { .mode = "exec-dfl",          .sig = SIGINT,  .dies = SIGINT  },
        // A caught note unwinds the wait.
        { .mode = "intr-norestart",    .sig = SIGUSR1, .repost = 1 },
        // And the kernel re-issues it for SA_RESTART. The handler acks before the
        // data goes, so the handler ran during the wait and not after it.
        { .mode = "intr-restart",      .sig = SIGUSR1, .ack = 1, .data = 1 },
        { .mode = "signal-restart",    .sig = SIGUSR2, .ack = 1, .data = 1 },
        // A default-ignored note and an ignored one stay out of the wait.
        { .mode = "quiet",             .sig = SIGCHLD, .also = SIGINT, .data = 1 },
        { .mode = "pause",             .sig = SIGUSR1, .repost = 1 },
        { .mode = "nanosleep",         .sig = SIGUSR1, .repost = 1 },
        { .mode = "wait-norestart",    .sig = SIGUSR1, .repost = 1 },
        { .mode = "wait-restart",      .sig = SIGUSR1 },
        // The table and the handler cross a fork.
        { .mode = "fork-read",         .data = 1 },
        // A handler that longjmp'd out never told the kernel it was done, and a fatal
        // note with a default row must still end the program, not spin its waits.
        { .mode = "escape",            .sig = SIGINT,  .dies = SIGINT  },
        // A dead child's note takes no room in a program with no handler.
        { .mode = "ring-room",         .data = 1 },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        if (run_blocked_leg(&legs[i]) != 0) bad = 1;
        fflush(stdout);
    }
    return bad ? -1 : 0;
}

// The kernel sleeps a poll over no fds. These answered ENOSYS, and pause() sits on
// the same sleep.
static int test_empty_poll(void) {
    const long long want = 30;
    long long t0 = leg_now_ms();
    int r1 = poll(NULL, 0, 40);
    long long t1 = leg_now_ms();
    struct pollfd neg = { -1, POLLIN, 0 };
    int r2 = poll(&neg, 1, 40);
    long long t2 = leg_now_ms();
    struct timeval tv = { 0, 40000 };
    int r3 = select(0, NULL, NULL, NULL, &tv);
    long long t3 = leg_now_ms();
    if (r1 != 0 || r2 != 0 || r3 != 0 ||
        t1 - t0 < want || t2 - t1 < want || t3 - t2 < want) {
        printf("pouch-hello-signals: empty poll: rc %d %d %d after %lld %lld %lld ms errno=%d\n",
               r1, r2, r3, t1 - t0, t2 - t1, t3 - t2, errno);
        return -1;
    }
    printf("pouch-hello-signals: a poll or select over no fds sleeps for its timeout\n");
    return 0;
}

// Task #96 sentinel buffers. 16-byte aligned for STP/LDP-Q.
static unsigned char g_fp_sent[512] __attribute__((aligned(16)));
static unsigned char g_fp_seen[512] __attribute__((aligned(16)));

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "die")) return die(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "kill-parent")) return kill_parent(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "leg")) return leg_child(argv[2]);

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
    // The kernel's row ignores it, so the handler does not run;
    // g_handler_count must stay at 2 (one from the raise above, one from the
    // #96 FP leg's self-post).
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
    int rc = sigaction(SIGALRM, &sa, NULL);
    if (rc == 0) {
        printf("pouch-hello-signals: sigaction(SIGALRM) unexpectedly succeeded\n");
        fflush(stdout);
        return 8;
    }
    if (errno != EINVAL) {
        printf("pouch-hello-signals: sigaction(SIGALRM) wrong errno=%d (expected EINVAL=%d)\n",
               errno, EINVAL);
        fflush(stdout);
        return 9;
    }
    printf("pouch-hello-signals: unsupported sigaction returns EINVAL\n");
    fflush(stdout);

    if (test_usr_apart() != 0) {
        fflush(stdout);
        return 17;
    }

    if (test_term_apart() != 0) {
        fflush(stdout);
        return 13;
    }
    fflush(stdout);

    // The death legs are separate processes: both groups run before either
    // verdict, so a failure in one does not hide the other.
    int deaths = test_deaths();
    int term_death = test_kill_death(SIGTERM, "term", "SIGTERM");
    int usr_death  = test_kill_death(SIGUSR1, "usr1", "SIGUSR1") |
                     test_kill_death(SIGUSR2, "usr2", "SIGUSR2");
    int errnos = test_errnos();
    fflush(stdout);
    int blocked = test_blocked();
    int empty = test_empty_poll();
    fflush(stdout);
    if (blocked != 0) return 19;
    if (empty != 0) return 20;
    if (deaths != 0) return 15;
    if (term_death != 0) return 14;
    if (usr_death != 0) return 18;
    if (errnos != 0) return 16;

    printf("%s\n", POUCH_CENSUS_SIGNALS);
    fflush(stdout);
    return 0;
}
