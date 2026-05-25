// /pouch-hello-argv — the argv-pass-through pouch hello (Phase 6 sub-chunk 16b-alpha).
//
// Proves the new SYS_SPAWN_FULL_ARGV kernel surface: joey spawns this
// binary with a constructed argv (e.g. ["pouch-hello-argv", "alpha",
// "beta", "gamma"]); pouch's musl _start parses the System V startup
// frame the kernel built (via exec_build_init_stack's Shape B) and
// exposes argc + argv to main; this main prints each argument back so
// joey can content-check the round-trip.
//
// The marker lines are line-prefixed with the argv index so joey's
// substring match can pin each one individually:
//   pouch-hello-argv: argc=4
//   pouch-hello-argv: argv[0]=pouch-hello-argv
//   pouch-hello-argv: argv[1]=alpha
//   pouch-hello-argv: argv[2]=beta
//   pouch-hello-argv: argv[3]=gamma
//   pouch-hello-argv: exit 0
//
// What this binary does NOT test: pthread / sockets / poll / signals —
// those are owned by their dedicated pouch-hello binaries. This one is
// the minimum-surface probe for argv delivery.

#include <stdio.h>

int main(int argc, char **argv) {
    if (printf("pouch-hello-argv: argc=%d\n", argc) < 0)
        return 1;
    for (int i = 0; i < argc; i++) {
        if (argv[i] == 0) {
            (void)printf("pouch-hello-argv: argv[%d]=<NULL>\n", i);
            return 1;
        }
        if (printf("pouch-hello-argv: argv[%d]=%s\n", i, argv[i]) < 0)
            return 1;
    }
    // R1 F2 fix: assert the POSIX argv[argc] == NULL guarantee. The
    // kernel writes the terminator at w[1 + argc] in exec_build_init_
    // stack's Shape B; if a future refactor dropped that write, this
    // probe would silently exit 0 — so we explicitly read argv[argc]
    // and fail loudly on a non-NULL.
    if (argv[argc] != 0) {
        (void)printf("pouch-hello-argv: argv[argc]=%p (expected NULL)\n",
                     (void *)argv[argc]);
        return 1;
    }
    if (puts("pouch-hello-argv: argv[argc] NULL terminator ok") < 0)
        return 1;
    if (puts("pouch-hello-argv: exit 0") < 0)
        return 1;
    return 0;
}
