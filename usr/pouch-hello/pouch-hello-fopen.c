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
//   tmpfile  : tmpfile() is delete-on-close: three pages round-trip to a
//              clean EOF over the wire, and the /tmp name is gone after
//              fclose() and after a child's exit(). (This leg used to claim
//              "the fid survives the unlink". It does not on this root
//              filesystem -- Stratum rejects I/O on an unlinked fid -- and
//              the leg was green only because libc never issued the unlink.)
//   scan     : fscanf over a real FILE, three pushbacks deep (0035): the
//              read backend must leave the last byte at rpos[-1] or every
//              pushed-back delimiter is re-read as a stale buffer byte.
//
// fd 1 is a pipe write-end joey relays to the boot log. Cross-compiled
// with tools/pouch-clang against the pouch sysroot.

#include "pouch-census.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#define PROBE "/pouch-fopen-probe.txt"
#define PROBE2 "/pouch-fopen-probe2.txt"
#define SELF "/bin/pouch-hello-fopen"

extern char **environ;

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
    {
        int c = fgetc(f);
        if (c != EOF || !feof(f)) {
            // Say what the stream and the fd each think: a FAIL with no evidence
            // costs a second boot to understand.
            int se = errno, fe = feof(f), fr = ferror(f);
            char one;
            errno = 0;
            long r = (long)read(fileno(f), &one, 1);
            printf("pouch-hello-fopen: scan[%lu] at EOF: fgetc=%d feof=%d ferror=%d errno=%d; "
                   "raw read=%ld errno=%d\n", (unsigned long)vsize, c, fe, fr, se, r, errno);
            return fail("scan EOF");
        }
    }

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

int main(int argc, char **argv) {
    // CHILD (self-respawn, the tmpfile exit leg): leave a tmpfile OPEN and return.
    if (argc >= 2 && !strcmp(argv[1], "tmpleak")) {
        // 42, not 0: the parent must see that THIS arm ran (argv arrived), not
        // merely that some run of this binary exited cleanly.
        FILE *t = tmpfile();
        if (!t || fputs("left open on purpose\n", t) == EOF) return 2;
        return 42;
    }

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
    // O_APPEND means EVERY write lands at end-of-file, not just the first one
    // after open. "a+", seek to 0, write: the bytes must follow "beta". With
    // the one-time-seek emulation they overwrote "alpha" in place.
    f = fopen(PROBE, "a+");
    if (!f) return fail("append fopen(a+)");
    if (fseek(f, 0, SEEK_SET)) return fail("append fseek");
    if (fputs("gamma\n", f) == EOF) return fail("append fputs after seek");
    if (fclose(f)) return fail("append a+ fclose");
    f = fopen(PROBE, "r");
    if (!f) return fail("append a+ reopen");
    {
        char all[64];
        size_t k = fread(all, 1, sizeof all - 1, f);
        all[k] = 0;
        if (strcmp(all, "alpha\nbeta\ngamma\n")) {
            printf("pouch-hello-fopen: after a+ / seek 0 / write the file reads \"%s\"\n", all);
            return fail("O_APPEND write did not land at end-of-file");
        }
    }
    if (fclose(f)) return fail("append a+ verify fclose");
    puts("pouch-hello-fopen: append OK (a write after a seek still lands at EOF)");

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

    // tmpfile: delete-on-close (pouch 0036). Three claims, each checked by
    // COUNTING /tmp/tmpfile_* names rather than by "absent" -- a preserved pool
    // may carry residue from before the fix, and only growth across THESE calls
    // is these calls' doing:
    //   1. the stream WORKS past the page cache: three pages out, back to a clean
    //      EOF. (The EOF probe is a wire read; on a file already unlinked the
    //      filesystem refuses it -- which is why upstream's create-then-unlink
    //      cannot be used here, and what the first version of this fix hit.)
    //   2. fclose() removes the name.
    //   3. a stream still open at exit() has its name removed too: a child is
    //      respawned with "tmpleak", opens a tmpfile and returns from main.
    int tmp_before = count_tmpfiles();
    if (tmp_before < 0) return fail("tmpfile opendir /tmp (before)");
    f = tmpfile();
    if (!f) return fail("tmpfile");
    {
        enum { TMP_BYTES = 3 * 4096 + 5 };
        static unsigned char out[TMP_BYTES], in[TMP_BYTES];
        for (unsigned i = 0; i < TMP_BYTES; i++) out[i] = (unsigned char)(i * 131u + 7u);
        if (fwrite(out, 1, TMP_BYTES, f) != TMP_BYTES) return fail("tmpfile fwrite");
        if (fflush(f)) return fail("tmpfile fflush");
        rewind(f);
        size_t got = 0, k;
        while ((k = fread(in + got, 1, TMP_BYTES - got, f)) > 0) got += k;
        if (got != TMP_BYTES || memcmp(in, out, TMP_BYTES)) {
            printf("pouch-hello-fopen: tmpfile read back %lu of %d, ferror=%d errno=%d\n",
                   (unsigned long)got, TMP_BYTES, ferror(f), errno);
            return fail("tmpfile round trip");
        }
        if (fgetc(f) != EOF || !feof(f) || ferror(f)) {
            printf("pouch-hello-fopen: tmpfile EOF: feof=%d ferror=%d errno=%d\n",
                   feof(f), ferror(f), errno);
            return fail("tmpfile clean EOF");
        }
        // The positive control for the two counts below: while the stream is
        // open the counter must SEE the name. A counter blind to it (a changed
        // prefix or directory) would read "unchanged" on a libc that leaks.
        int during = count_tmpfiles();
        if (during != tmp_before + 1) {
            printf("pouch-hello-fopen: /tmp tmpfile_* names %d -> %d while one is open\n",
                   tmp_before, during);
            return fail("the tmpfile counter cannot see an open tmpfile");
        }
    }
    if (fclose(f)) return fail("tmpfile fclose");
    {
        int n = count_tmpfiles();
        if (n != tmp_before) {
            printf("pouch-hello-fopen: /tmp tmpfile_* names %d -> %d across tmpfile+fclose\n",
                   tmp_before, n);
            return fail("tmpfile left its name after fclose");
        }
    }
    {
        char *cargv[] = { (char *)SELF, (char *)"tmpleak", NULL };
        pid_t pid;
        int st = 0;
        if (posix_spawn(&pid, SELF, NULL, NULL, cargv, environ) != 0)
            return fail("tmpfile respawn");
        if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 42) {
            printf("pouch-hello-fopen: tmpleak child status %#x\n", st);
            return fail("tmpfile child");
        }
        int n = count_tmpfiles();
        if (n != tmp_before) {
            printf("pouch-hello-fopen: /tmp tmpfile_* names %d -> %d across a child's exit\n",
                   tmp_before, n);
            return fail("tmpfile left its name after exit()");
        }
    }
    puts("pouch-hello-fopen: tmpfile OK");

    // scan: three passes over the same text. The default buffer; then a setvbuf()
    // buffer of UNGET+1 and UNGET+2 bytes (musl keeps UNGET = 8 of them for
    // pushback, so the stream buffer is ONE and TWO bytes) -- the sizes at which a
    // one-byte request meets the read backend's buffered/direct boundary.
    if (scan_pass(0) || scan_pass(9) || scan_pass(10)) return 1;
    puts("pouch-hello-fopen: scan OK");

    // The census is what joey matches: a stale binary prints the old marker.
    puts(POUCH_CENSUS_FOPEN);
    return 0;
}
