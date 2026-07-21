// /pouch-hello-fopen — the create-mode fopen / unlink-family prover
// (task #50; the 0024-pouch-fopen-create boundary-line).
//
// Runs POST-PIVOT (spawned as /bin/pouch-hello-fopen) so every leg lands
// on the persistent Stratum FS — the surface Quake's config.cfg rides.
// Legs, each printed + boot-fatal via the joey expect string:
//
//   create   : fopen("w") on an ABSENT path — openat's O_CREAT arm
//              (parent T_OPATH open + SYS_WALK_CREATE) — write, close,
//              re-open "r", verify.
//   append   : fopen("a") — O_CREAT on the now-EXISTING file takes the
//              plain-open fast path + the O_APPEND seek-to-END; both
//              lines must survive.
//   truncate : fopen("w") again — the existing-file path must carry
//              T_OTRUNC (the 0021 silent no-op made fopen("w")
//              overwrite-in-place); ONLY the new content may remain.
//   excl     : open(O_CREAT|O_EXCL) on the existing path — EEXIST.
//   unlink   : unlink() (the unlinkat boundary-line: parent T_OPATH +
//              SYS_UNLINK) — a re-open must ENOENT.
//   remove   : remove() on a fresh file — the stdio-facing arm.
//   tmpfile  : tmpfile() — O_CREAT|O_EXCL under /tmp + the immediate
//              unlink; write/rewind/read AFTER the unlink proves the
//              Plan 9-lineage fid-survives-unlink property end to end
//              (the open fid keeps the file alive until clunk).
//
// fd 1 is a pipe write-end joey relays to the boot log. Cross-compiled
// with tools/pouch-clang against the pouch sysroot.

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define PROBE "/pouch-fopen-probe.txt"
#define PROBE2 "/pouch-fopen-probe2.txt"

static int fail(const char *leg) {
    printf("pouch-hello-fopen: FAIL %s (errno %d)\n", leg, errno);
    return 1;
}

int main(void) {
    // create
    FILE *f = fopen(PROBE, "w");
    if (!f) return fail("create fopen(w)");
    if (fputs("alpha\n", f) == EOF) return fail("create fputs");
    if (fclose(f)) return fail("create fclose");
    f = fopen(PROBE, "r");
    if (!f) return fail("create reopen(r)");
    char buf[64];
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, "alpha\n"))
        return fail("create verify");
    if (fclose(f)) return fail("create verify fclose");
    puts("pouch-hello-fopen: create OK");

    // append
    f = fopen(PROBE, "a");
    if (!f) return fail("append fopen(a)");
    if (fputs("beta\n", f) == EOF) return fail("append fputs");
    if (fclose(f)) return fail("append fclose");
    f = fopen(PROBE, "r");
    if (!f) return fail("append reopen");
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, "alpha\n"))
        return fail("append line1");
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, "beta\n"))
        return fail("append line2");
    if (fclose(f)) return fail("append verify fclose");
    puts("pouch-hello-fopen: append OK");

    // truncate
    f = fopen(PROBE, "w");
    if (!f) return fail("trunc fopen(w)");
    if (fputs("gamma\n", f) == EOF) return fail("trunc fputs");
    if (fclose(f)) return fail("trunc fclose");
    f = fopen(PROBE, "r");
    if (!f) return fail("trunc reopen");
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, "gamma\n"))
        return fail("trunc line1");
    if (fgets(buf, sizeof buf, f) != NULL) return fail("trunc not-truncated");
    if (fclose(f)) return fail("trunc verify fclose");
    puts("pouch-hello-fopen: truncate OK");

    // excl
    errno = 0;
    int fd = open(PROBE, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd >= 0 || errno != EEXIST) return fail("excl not-EEXIST");
    puts("pouch-hello-fopen: excl OK");

    // unlink
    if (unlink(PROBE)) return fail("unlink");
    errno = 0;
    f = fopen(PROBE, "r");
    if (f || errno != ENOENT) return fail("unlink reopen not-ENOENT");
    puts("pouch-hello-fopen: unlink OK");

    // remove
    f = fopen(PROBE2, "w");
    if (!f) return fail("remove fopen(w)");
    if (fclose(f)) return fail("remove fclose");
    if (remove(PROBE2)) return fail("remove");
    errno = 0;
    f = fopen(PROBE2, "r");
    if (f || errno != ENOENT) return fail("remove reopen not-ENOENT");
    puts("pouch-hello-fopen: remove OK");

    // tmpfile (write/rewind/read AFTER the immediate unlink)
    f = tmpfile();
    if (!f) return fail("tmpfile");
    if (fputs("delta\n", f) == EOF) return fail("tmpfile fputs");
    if (fflush(f)) return fail("tmpfile fflush");
    rewind(f);
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, "delta\n"))
        return fail("tmpfile verify");
    if (fclose(f)) return fail("tmpfile fclose");
    puts("pouch-hello-fopen: tmpfile OK");

    puts("pouch-hello-fopen: exit 0");
    return 0;
}
