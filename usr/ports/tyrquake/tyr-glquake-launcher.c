/* tyr-glquake -- the ramfs face of the pool binary (/clade/bin/tyr-glquake).
 * The GL build is pool-resident (too large for ramfs economy); this launcher
 * makes the bare name resolve from the shell exactly like tyr-quake does.
 * Superseded by a union bind of /clade/bin onto /bin when MAFTER walking
 * lands (territory.h reserves it for v1.x). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#define TARGET "/clade/bin/tyr-glquake"

#define THYLA_SYS_OPEN  65
#define THYLA_FROM_ROOT (-1L)
#define THYLA_OREAD     0

static long thyla_open(long dirfd, const char *name, unsigned long len,
                       unsigned long omode)
{
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)(uintptr_t)name;
    register long x2 __asm__("x2") = (long)len;
    register long x3 __asm__("x3") = (long)omode;
    register long x8 __asm__("x8") = THYLA_SYS_OPEN;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory", "cc");
    return x0;
}

/* Warp-4: prefer the GPU when the box has one. The probe is virgl_prove.c's
 * warp_seam_present shape, raw syscalls included: the /srv/warp post is an
 * attach point, not a directory, so a one-shot pouch open("/srv/warp/ctl")
 * CANNOT cross it (probed, not assumed) -- open the post (attach), walk ctl
 * from the conn root, and read the leading "virgl 1" that says the host GL
 * came up. Pouch fds are kernel handles 1:1, so musl read/close work on
 * them. The choice rides a /env write, not setenv: pouch environ is a
 * startup snapshot OF /env (patch 0025), so only the device write survives
 * the exec below and clones into a spawned child. An explicit
 * GALLIUM_DRIVER anywhere in the inherited environment wins -- this is a
 * default, not an override; osmesa still falls back loudly if the seam
 * dies before screen create. */
static void prefer_virgl_when_present(void)
{
    char buf[16];
    long root, ctl;
    ssize_t n;
    int fd;

    if (getenv("GALLIUM_DRIVER"))
        return;
    root = thyla_open(THYLA_FROM_ROOT, "/srv/warp", 9, THYLA_OREAD);
    if (root < 0)
        return;
    ctl = thyla_open(root, "ctl", 3, THYLA_OREAD);
    close((int)root);
    if (ctl < 0)
        return;
    n = read((int)ctl, buf, sizeof(buf) - 1);
    close((int)ctl);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strncmp(buf, "virgl 1", 7) != 0)
        return;
    fd = open("/env/GALLIUM_DRIVER", O_WRONLY | O_CREAT, 0666);
    if (fd < 0)
        return;
    if (write(fd, "virpipe", 7) != 7)
        fprintf(stderr, "tyr-glquake: short /env write -- "
                        "staying on the software rasteriser\n");
    close(fd);
}

static pid_t g_child;

static void forward_interrupt(int sig)
{
    if (g_child > 0)
        kill(g_child, sig);
}

int main(int argc, char **argv)
{
    (void)argc;
    prefer_virgl_when_present();
    execv(TARGET, argv);

    /* execv returned: pouch has no execve until LINEAGE L-6 (patch 0026),
     * so the spawn fallback is today's ONLY arm -- which makes this
     * wrapper the console OWNER interposed above the game, and the serial
     * console's ^C posts its interrupt to the OWNER ALONE (the pre-pgrp
     * LS-5 shape; pgrp fan-out exists only behind the PTY seam, #197).
     * Uncaught, the ^C would kill this wrapper and orphan a fullscreen
     * game holding the scanout -- the Warp-4c gate caught exactly that.
     * Catch and forward instead: the child's own default-terminate does
     * the dying, and the reap below returns its status. Handlers go in
     * BEFORE the spawn so no window orphans; a ^C landing in the
     * handler-armed-but-child-unrecorded gap is merely dropped. */
    signal(SIGINT, forward_interrupt);
    signal(SIGTERM, forward_interrupt);
    pid_t pid = 0;
    if (posix_spawn(&pid, TARGET, NULL, NULL, argv, NULL) == 0) {
        int st = 0;
        g_child = pid;
        /* WNOHANG + a short sleep instead of a blocking wait: a CAUGHT note
         * delivers only at an EL0-return (#199 -- the kernel wakes blocked
         * threads for the terminate latch alone), so a blocking waitpid
         * here would deadlock: the handler that kills the child can never
         * run while this thread sleeps waiting for the child to die. The
         * poll cadence IS the delivery point; 50 ms bounds the ^C latency. */
        for (;;) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid)
                break;
            if (r < 0 && errno != EINTR)
                return 1;
            usleep(50000);
        }
        if (WIFEXITED(st))
            return WEXITSTATUS(st);
        return 1;
    }
    fprintf(stderr,
            "tyr-glquake: " TARGET " not reachable (is the clade pool mounted?)\n");
    return 127;
}
