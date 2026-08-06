/* tyr-glquake -- the ramfs face of the pool binary (/clade/bin/tyr-glquake).
 * The GL build is pool-resident (too large for ramfs economy); this launcher
 * makes the bare name resolve from the shell exactly like tyr-quake does.
 * Superseded by a union bind of /clade/bin onto /bin when MAFTER walking
 * lands (territory.h reserves it for v1.x). */
#include <stdio.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#define TARGET "/clade/bin/tyr-glquake"

int main(int argc, char **argv)
{
    (void)argc;
    execv(TARGET, argv);

    /* execv returned: the exec seam may be unwired for this path -- try the
     * spawn fallback before concluding the target is absent. */
    pid_t pid = 0;
    if (posix_spawn(&pid, TARGET, NULL, NULL, argv, NULL) == 0) {
        int st = 0;
        if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
            return WEXITSTATUS(st);
        return 1;
    }
    fprintf(stderr,
            "tyr-glquake: " TARGET " not reachable (is the clade pool mounted?)\n");
    return 127;
}
