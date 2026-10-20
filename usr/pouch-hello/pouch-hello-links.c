// /pouch-hello-links -- the symlink prover (B-3f'; patch 0059, operator vote
// 2026-10-20: native SYS_LSTAT 136 and SYS_READLINK 135).
//
// Runs POST-PIVOT (spawned as /bin/pouch-hello-links) over real links the bake
// puts in the pool at /lib/pouch-links (tools/build.sh, populate_stratum_pool):
//
//   target     a file          live  -> target     dead -> nosuch
//   sub/leaf   a file          dir   -> sub        loop -> loop
//                              abs   -> /lib/pouch-links/target
//                              chain -> live
//
// Pouch cannot make a link (symlink() is ENOSYS), so every leg only reads the
// fixture; the two that create a name make it at the root and remove it. Legs,
// each printed + boot-fatal via the joey expect string:
//
//   lstat           : lstat reports each link itself, S_IFLNK with the
//                     target's length as its size, where stat follows it to
//                     the file, the directory, ENOENT or ELOOP; lstat of a
//                     file is the file; fstatat AT_SYMLINK_NOFOLLOW agrees.
//   readlink        : each link's target, byte for byte and not terminated;
//                     EINVAL for a file, a directory, and a file reached
//                     through a link; ENOENT for an absent path.
//   readlink-trunc  : a short buffer takes the first bytes and nothing past
//                     them; a zero-size buffer answers 0 for a link (musl's
//                     dummy byte) and EINVAL for a file.
//   realpath        : a link, a chain of two, an absolute link and a link in
//                     the middle of the path expand to the real path; a
//                     dangling link is ENOENT and a self-loop ELOOP.
//   nofollow        : O_NOFOLLOW on a final link is ELOOP; on a file it opens,
//                     and a link earlier in the path is still followed.
//   opath           : an O_PATH descriptor of a link is the file it names
//                     (fstat) and refuses a read, EBADF; with O_NOFOLLOW it is
//                     the link, dangling or not.
//   directory       : O_DIRECTORY on a file is ENOTDIR, and so is opendir; a
//                     link to a directory opens and lists. With O_NOFOLLOW a
//                     link is ELOOP (current Linux answers ENOTDIR; the
//                     kernel refuses the link first), and with O_PATH as well
//                     it is ENOTDIR, as on Linux.
//   creat-directory : O_CREAT with O_DIRECTORY is EINVAL and creates nothing;
//                     O_PATH drops O_CREAT, so a missing name is ENOENT.
//   lchmod          : lchmod and fchmodat AT_SYMLINK_NOFOLLOW of a link are
//                     EOPNOTSUPP and leave the target's mode alone; on a file
//                     that is not a link, NOFOLLOW still sets the mode. The
//                     link legs pass the target's own mode, so an old libc
//                     that follows changes nothing.
//   flags           : fstatat and fchmodat refuse a flag they do not know,
//                     EINVAL.
//
// Every leg was red on the libc before 0059, which followed every link and
// dropped O_NOFOLLOW, O_PATH and O_DIRECTORY. Each leg runs, so a red run
// names each failing leg and only those; the census line prints only when all
// pass. fd 1 is a pipe write-end joey relays to the boot log, line-buffered.
// Cross-compiled with tools/pouch-clang against the pouch sysroot.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIX "/lib/pouch-links"
#define TARGET_TEXT "pouch-links target\n"
#define CREAT_PROBE "/pouch-links-creat"
#define CHMOD_PROBE "/pouch-links-chmod"

static const struct { const char *path; const char *target; } links[] = {
    { FIX "/live", "target" },          { FIX "/dead", "nosuch" },
    { FIX "/dir", "sub" },              { FIX "/abs", FIX "/target" },
    { FIX "/loop", "loop" },            { FIX "/chain", "live" },
};
#define NLINKS (sizeof links / sizeof links[0])

static int fail(const char *what) {
    printf("pouch-hello-links: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

// The whole of a small file, as text; -1 if it cannot be read.
static long slurp(int fd, char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0) return -1;
        if (r == 0) break;
        n += (size_t)r;
        if (n == cap - 1) break;
    }
    buf[n] = '\0';
    return (long)n;
}

static int leg_lstat(void) {
    struct stat st;
    for (size_t i = 0; i < NLINKS; i++) {
        if (lstat(links[i].path, &st) != 0) return fail(links[i].path);
        if (!S_ISLNK(st.st_mode)) return fail("lstat did not report a link");
        if (st.st_size != (off_t)strlen(links[i].target))
            return fail("lstat size is not the target's length");
    }
    if (stat(FIX "/live", &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != (off_t)(sizeof TARGET_TEXT - 1))
        return fail("stat did not follow live to its file");
    if (stat(FIX "/dir", &st) != 0 || !S_ISDIR(st.st_mode))
        return fail("stat did not follow dir to its directory");
    errno = 0;
    if (stat(FIX "/dead", &st) == 0 || errno != ENOENT)
        return fail("stat of a dangling link is not ENOENT");
    errno = 0;
    if (stat(FIX "/loop", &st) == 0 || errno != ELOOP)
        return fail("stat of a self-loop is not ELOOP");
    if (lstat(FIX "/target", &st) != 0 || !S_ISREG(st.st_mode))
        return fail("lstat of a file is not the file");
    if (fstatat(AT_FDCWD, FIX "/live", &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(st.st_mode))
        return fail("fstatat AT_SYMLINK_NOFOLLOW did not report the link");
    return 0;
}

static int leg_readlink(void) {
    char buf[64];
    for (size_t i = 0; i < NLINKS; i++) {
        size_t want = strlen(links[i].target);
        memset(buf, 'Z', sizeof buf);
        ssize_t n = readlink(links[i].path, buf, sizeof buf);
        if (n < 0) return fail(links[i].path);
        if ((size_t)n != want || memcmp(buf, links[i].target, want) != 0)
            return fail("readlink answered another target");
        if (buf[n] != 'Z') return fail("readlink wrote past the target");
    }
    static const char *const not_links[] = { FIX "/target", FIX "/sub", FIX "/dir/leaf" };
    for (size_t i = 0; i < sizeof not_links / sizeof not_links[0]; i++) {
        errno = 0;
        if (readlink(not_links[i], buf, sizeof buf) != -1 || errno != EINVAL)
            return fail("readlink of what is not a link is not EINVAL");
    }
    errno = 0;
    if (readlink(FIX "/nope", buf, sizeof buf) != -1 || errno != ENOENT)
        return fail("readlink of an absent path is not ENOENT");
    return 0;
}

static int leg_readlink_trunc(void) {
    char buf[16];
    memset(buf, 'Z', sizeof buf);
    if (readlink(FIX "/abs", buf, 4) != 4) return fail("readlink did not fill a short buffer");
    if (memcmp(buf, "/lib", 4) != 0) return fail("readlink truncated to other bytes");
    if (buf[4] != 'Z') return fail("readlink wrote past a short buffer");
    if (readlink(FIX "/live", buf, 0) != 0) return fail("readlink of a link, size 0, is not 0");
    errno = 0;
    if (readlink(FIX "/target", buf, 0) != -1 || errno != EINVAL)
        return fail("readlink of a file, size 0, is not EINVAL");
    return 0;
}

static int leg_realpath(void) {
    static const struct { const char *path, *real; } rows[] = {
        { FIX "/live", FIX "/target" },     { FIX "/chain", FIX "/target" },
        { FIX "/abs", FIX "/target" },      { FIX "/dir/leaf", FIX "/sub/leaf" },
    };
    char rp[PATH_MAX];      // musl copies a whole PATH_MAX out
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        if (!realpath(rows[i].path, rp)) return fail(rows[i].path);
        if (strcmp(rp, rows[i].real) != 0) return fail("realpath left a link unexpanded");
    }
    errno = 0;
    if (realpath(FIX "/dead", rp) || errno != ENOENT)
        return fail("realpath of a dangling link is not ENOENT");
    errno = 0;
    if (realpath(FIX "/loop", rp) || errno != ELOOP)
        return fail("realpath of a self-loop is not ELOOP");
    return 0;
}

static int leg_nofollow(void) {
    char buf[64];
    errno = 0;
    int fd = open(FIX "/live", O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) { close(fd); return fail("O_NOFOLLOW opened a link"); }
    if (errno != ELOOP) return fail("O_NOFOLLOW on a link is not ELOOP");
    fd = open(FIX "/target", O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return fail("O_NOFOLLOW refused a file");
    long n = slurp(fd, buf, sizeof buf);
    close(fd);
    if (n < 0 || strcmp(buf, TARGET_TEXT) != 0) return fail("O_NOFOLLOW read another file");
    fd = open(FIX "/live", O_RDONLY);
    if (fd < 0) return fail("open did not follow a link");
    n = slurp(fd, buf, sizeof buf);
    close(fd);
    if (n < 0 || strcmp(buf, TARGET_TEXT) != 0) return fail("open of a link read another file");
    fd = open(FIX "/dir/leaf", O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return fail("O_NOFOLLOW refused a link earlier in the path");
    close(fd);
    return 0;
}

static int leg_opath(void) {
    struct stat st;
    char c;
    int fd = open(FIX "/live", O_PATH);
    if (fd < 0) return fail("O_PATH open of a link");
    int bad = fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
              st.st_size != (off_t)(sizeof TARGET_TEXT - 1);
    errno = 0;
    ssize_t r = read(fd, &c, 1);
    int e = errno;
    close(fd);
    if (bad) return fail("O_PATH of a link is not the file it names");
    if (r != -1 || e != EBADF) { errno = e; return fail("read on an O_PATH descriptor is not EBADF"); }
    static const char *const held[] = { FIX "/live", FIX "/dead" };
    for (size_t i = 0; i < sizeof held / sizeof held[0]; i++) {
        fd = open(held[i], O_PATH | O_NOFOLLOW);
        if (fd < 0) return fail(held[i]);
        bad = fstat(fd, &st) != 0 || !S_ISLNK(st.st_mode);
        close(fd);
        if (bad) return fail("O_PATH|O_NOFOLLOW is not the link");
    }
    return 0;
}

// -1 if fd >= 0 (closed) or errno != want; 0 otherwise.
static int refused(int fd, int want) {
    int e = errno;
    if (fd >= 0) { close(fd); return -1; }
    return e == want ? 0 : -1;
}

static int leg_directory(void) {
    errno = 0;
    if (refused(open(FIX "/target", O_RDONLY | O_DIRECTORY), ENOTDIR))
        return fail("O_DIRECTORY on a file is not ENOTDIR");
    int fd = open(FIX "/dir", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fail("O_DIRECTORY did not follow a link to its directory");
    close(fd);
    errno = 0;
    if (refused(open(FIX "/dir", O_RDONLY | O_DIRECTORY | O_NOFOLLOW), ELOOP))
        return fail("O_DIRECTORY|O_NOFOLLOW on a link is not ELOOP");
    errno = 0;
    if (refused(open(FIX "/dir", O_PATH | O_DIRECTORY | O_NOFOLLOW), ENOTDIR))
        return fail("O_PATH|O_DIRECTORY|O_NOFOLLOW on a link is not ENOTDIR");
    fd = open(FIX "/sub", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return fail("O_DIRECTORY|O_NOFOLLOW refused a directory");
    close(fd);
    errno = 0;
    DIR *d = opendir(FIX "/target");
    if (d) { closedir(d); return fail("opendir opened a file"); }
    if (errno != ENOTDIR) return fail("opendir of a file is not ENOTDIR");
    d = opendir(FIX "/dir");
    if (!d) return fail("opendir did not follow a link");
    int leaf = 0;
    for (struct dirent *de; (de = readdir(d)); )
        leaf += strcmp(de->d_name, "leaf") == 0;
    closedir(d);
    if (leaf != 1) return fail("opendir through a link did not list leaf");
    return 0;
}

static int leg_creat_directory(void) {
    struct stat st;
    unlink(CREAT_PROBE);
    errno = 0;
    int fd = open(CREAT_PROBE, O_RDWR | O_CREAT | O_DIRECTORY, 0600);
    int e = errno;
    if (fd >= 0) close(fd);
    int made = lstat(CREAT_PROBE, &st) == 0;
    unlink(CREAT_PROBE);
    if (fd >= 0 || made) return fail("O_CREAT|O_DIRECTORY created a name");
    if (e != EINVAL) { errno = e; return fail("O_CREAT|O_DIRECTORY is not EINVAL"); }
    errno = 0;
    fd = open(CREAT_PROBE, O_PATH | O_CREAT, 0600);
    e = errno;
    if (fd >= 0) close(fd);
    made = lstat(CREAT_PROBE, &st) == 0;
    unlink(CREAT_PROBE);
    if (fd >= 0 || made) return fail("O_PATH|O_CREAT created a name");
    if (e != ENOENT) { errno = e; return fail("O_PATH|O_CREAT of a missing name is not ENOENT"); }
    return 0;
}

static int leg_lchmod(void) {
    struct stat t0, t1, st;
    if (stat(FIX "/target", &t0) != 0) return fail("stat target");
    mode_t keep = t0.st_mode & 0777;
    errno = 0;
    if (lchmod(FIX "/live", keep) != -1 || errno != EOPNOTSUPP)
        return fail("lchmod of a link is not EOPNOTSUPP");
    errno = 0;
    if (fchmodat(AT_FDCWD, FIX "/live", keep, AT_SYMLINK_NOFOLLOW) != -1 || errno != EOPNOTSUPP)
        return fail("fchmodat AT_SYMLINK_NOFOLLOW of a link is not EOPNOTSUPP");
    if (stat(FIX "/target", &t1) != 0 || t1.st_mode != t0.st_mode)
        return fail("lchmod changed the target's mode");
    int fd = open(CHMOD_PROBE, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return fail("create " CHMOD_PROBE);
    close(fd);
    int r = fchmodat(AT_FDCWD, CHMOD_PROBE, 0640, AT_SYMLINK_NOFOLLOW);
    int e = errno;
    int got = stat(CHMOD_PROBE, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
    unlink(CHMOD_PROBE);
    if (r != 0) { errno = e; return fail("fchmodat AT_SYMLINK_NOFOLLOW of a file"); }
    if (got != 0640) return fail("fchmodat AT_SYMLINK_NOFOLLOW did not set a file's mode");
    return 0;
}

static int leg_flags(void) {
    struct stat st;
    errno = 0;
    if (fstatat(AT_FDCWD, FIX "/target", &st, AT_RECURSIVE) != -1 || errno != EINVAL)
        return fail("fstatat accepted an unknown flag");
    if (stat(FIX "/target", &st) != 0) return fail("stat target");
    errno = 0;
    if (fchmodat(AT_FDCWD, FIX "/target", st.st_mode & 0777, AT_REMOVEDIR) != -1 || errno != EINVAL)
        return fail("fchmodat accepted an unknown flag");
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    static const struct { const char *name; int (*run)(void); } legs[] = {
        { "lstat", leg_lstat },                   { "readlink", leg_readlink },
        { "readlink-trunc", leg_readlink_trunc }, { "realpath", leg_realpath },
        { "nofollow", leg_nofollow },             { "opath", leg_opath },
        { "directory", leg_directory },           { "creat-directory", leg_creat_directory },
        { "lchmod", leg_lchmod },                 { "flags", leg_flags },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        if (legs[i].run()) failed++;
        else printf("pouch-hello-links: %s OK\n", legs[i].name);
    }
    if (failed) {
        printf("pouch-hello-links: %d leg(s) FAILED\n", failed);
        return 1;
    }
    puts(POUCH_CENSUS_LINKS);
    return 0;
}
