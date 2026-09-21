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
//   scan     : fscanf over a real FILE, three pushbacks deep (0035): the
//              read backend must leave the last byte at rpos[-1] or every
//              pushed-back delimiter is re-read as a stale buffer byte.
//
// fd 1 is a pipe write-end joey relays to the boot log. Cross-compiled
// with tools/pouch-clang against the pouch sysroot.

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define PROBE "/pouch-fopen-probe.txt"
#define PROBE2 "/pouch-fopen-probe2.txt"

// How many /tmp/tmpfile_* names exist right now; -1 if /tmp cannot be read.
static int count_tmpfiles(void) {
    DIR *d = opendir("/tmp");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (!strncmp(e->d_name, "tmpfile_", 8)) n++;
    closedir(d);
    return n;
}

static int fail(const char *leg) {
    printf("pouch-hello-fopen: FAIL %s (errno %d)\n", leg, errno);
    return 1;
}

// The scanf family pushes its terminating byte back with a bare `rpos--`, which
// only works if the read backend left that byte at rpos[-1]. Each step re-reads a
// pushed-back delimiter: the space after "alpha", the newline a "%d" stops at, and
// the byte fgetc must then see. Then a small fread() followed by a scan, which
// must continue from whatever the fread left buffered. Checks are on VALUES, so
// a scan that "succeeds" on the wrong bytes still fails.
static int scan_pass(size_t vsize) {
    static char vbuf[16];
    char line[64];
    char w1[8] = {0}, w2[8] = {0}, h[5];
    int a = 0, b = 0, n;
    unsigned x = 0;
    FILE *f = tmpfile();
    if (!f) return fail("scan tmpfile");
    if (vsize && setvbuf(f, vbuf, _IOFBF, vsize)) return fail("scan setvbuf");
    if (fputs("alpha 12 -7\n0x1f beta\ntail line\n", f) == EOF) return fail("scan fputs");
    if (fflush(f)) return fail("scan fflush");
    rewind(f);
    n = fscanf(f, "%7s %d %d", w1, &a, &b);
    if (n != 3 || strcmp(w1, "alpha") || a != 12 || b != -7) {
        printf("pouch-hello-fopen: scan[%lu] line1 n=%d w1=[%s] a=%d b=%d\n",
               (unsigned long)vsize, n, w1, a, b);
        return fail("scan line1 (fscanf pushback)");
    }
    if (fgetc(f) != '\n') return fail("scan pushed-back newline not re-read");
    n = fscanf(f, "%x %7s", &x, w2);
    if (n != 2 || x != 0x1f || strcmp(w2, "beta")) {
        printf("pouch-hello-fopen: scan[%lu] line2 n=%d x=%#x w2=[%s]\n",
               (unsigned long)vsize, n, x, w2);
        return fail("scan line2");
    }
    if (fgetc(f) != '\n') return fail("scan second pushed-back newline");
    if (!fgets(line, sizeof line, f) || strcmp(line, "tail line\n"))
        return fail("scan fgets after fscanf");
    if (fgetc(f) != EOF || !feof(f)) return fail("scan EOF");

    rewind(f);
    a = b = 0;
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "alpha", sizeof h))
        return fail("scan small fread");
    n = fscanf(f, "%d %d", &a, &b);
    if (n != 2 || a != 12 || b != -7) {
        printf("pouch-hello-fopen: scan[%lu] after fread n=%d a=%d b=%d\n",
               (unsigned long)vsize, n, a, b);
        return fail("scan after small fread");
    }
    if (fclose(f)) return fail("scan fclose");
    return 0;
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

    // tmpfile (write/rewind/read AFTER the immediate unlink). pouch 0036: the
    // unlink must actually HAPPEN -- for years it did not (a raw sentinel
    // syscall, result ignored), and this leg stayed green while proving nothing
    // about it. Counted, not "absent": a preserved pool may carry residue from
    // before the fix, and only growth across THIS call is this call's doing.
    int tmp_before = count_tmpfiles();
    if (tmp_before < 0) return fail("tmpfile opendir /tmp (before)");
    f = tmpfile();
    if (!f) return fail("tmpfile");
    {
        int tmp_after = count_tmpfiles();
        if (tmp_after != tmp_before) {
            printf("pouch-hello-fopen: /tmp tmpfile_* entries %d -> %d across tmpfile()\n",
                   tmp_before, tmp_after);
            return fail("tmpfile left a NAMED file (unlink not issued)");
        }
    }
    if (fputs("delta\n", f) == EOF) return fail("tmpfile fputs");
    if (fflush(f)) return fail("tmpfile fflush");
    rewind(f);
    if (!fgets(buf, sizeof buf, f) || strcmp(buf, "delta\n"))
        return fail("tmpfile verify");
    if (fclose(f)) return fail("tmpfile fclose");
    puts("pouch-hello-fopen: tmpfile OK");

    // scan: three passes over the same text. The default buffer; then a setvbuf()
    // buffer of UNGET+1 and UNGET+2 bytes (musl keeps UNGET = 8 of them for
    // pushback, so the stream buffer is ONE and TWO bytes) -- the sizes at which a
    // one-byte request meets the read backend's buffered/direct boundary.
    if (scan_pass(0) || scan_pass(9) || scan_pass(10)) return 1;
    puts("pouch-hello-fopen: scan OK");

    puts("pouch-hello-fopen: exit 0");
    return 0;
}
