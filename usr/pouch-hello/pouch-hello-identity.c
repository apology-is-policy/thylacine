// /pouch-hello-identity -- the A-6 witness: Pouch's identity calls tell the
// truth (IDENTITY-DESIGN section 9.10).
//
// THE BUG THIS DISCRIMINATES. musl's getuid/geteuid/getgid/getegid are
// CANNOT-FAIL by contract -- each is literally `return __syscall(SYS_x);` with
// no error path. Patch 0001 parks __NR_getuid & friends at the 0xFFFF
// unimplemented-syscall sentinel, so the sentinel's -ENOSYS was cast straight
// to uid_t and every Pouch program was told its uid is 0xFFFFFFDA. Not an
// error it could check: a LIE IT COULD NOT DETECT. Patch 0043 retargets the
// four onto SYS_GETUID (73) / SYS_GETGID (74).
//
// WHY IT COMPARES AGAINST /proc RATHER THAN A LITERAL. A test that asserted
// "getuid() == 4294967294" would pass for the wrong reason the moment the
// boot principal changed, and would tell us nothing about whether the call is
// wired to the kernel at all. So the probe reads the SAME Proc's identity back
// through a completely independent channel -- devproc's `principal:<N> gid:<M>`
// line in /proc/<pid>/status, which the kernel formats from p->principal_id and
// p->primary_gid -- and demands they AGREE. Two channels, one truth.
//
// It also names the specific defect: a dedicated assertion that the value is
// not the 0xFFFFFFDA sentinel, so a regression reports WHICH bug came back
// rather than merely "mismatch".
//
// effective == real is asserted rather than assumed. There is no setuid, no
// seteuid and no effective-uid field anywhere in the Thylacine kernel, which is
// invariant I-22 holding by construction (no ambient super-authority; elevation
// only via the legate). If a future change made geteuid() diverge from getuid()
// here, that is an I-22 violation before it is an ABI change, and this catches
// it.
//
// Output on success:
//   pouch-hello-identity: getpid -> <pid>
//   pouch-hello-identity: /proc says principal:<N> gid:<M>
//   pouch-hello-identity: getuid/geteuid -> <N>/<N> (ok, agrees with /proc)
//   pouch-hello-identity: getgid/getegid -> <M>/<M> (ok, agrees with /proc)
//   pouch-hello-identity: not the 0xFFFFFFDA ENOSYS sentinel (ok)
//   pouch-hello-identity: legs=getpid,proc-status,uid-agrees,gid-agrees,not-sentinel: exit 0
//
// Non-zero on any failed assertion -- joey treats that as a regression.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "pouch-census.h"

// The exact value the pre-0043 sentinel produced: (uid_t)(-ENOSYS).
#define ENOSYS_SENTINEL ((unsigned long)0xFFFFFFDAul)

// Parse "<key><digits>" out of buf; returns 0 on success.
static int field_after(const char *buf, const char *key, unsigned long *out)
{
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    if (*p < '0' || *p > '9') return -1;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10ul + (unsigned long)(*p++ - '0');
    *out = v;
    return 0;
}

int main(void)
{
    pid_t pid = getpid();
    printf("pouch-hello-identity: getpid -> %d\n", (int)pid);
    if (pid <= 0) {
        printf("pouch-hello-identity: FAIL -- getpid returned %d\n", (int)pid);
        return 1;
    }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("pouch-hello-identity: FAIL -- open(%s)\n", path);
        return 1;
    }
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) {
        printf("pouch-hello-identity: FAIL -- read(%s) -> %ld\n", path, (long)n);
        return 1;
    }
    buf[n] = '\0';

    unsigned long proc_uid = 0, proc_gid = 0;
    if (field_after(buf, "principal:", &proc_uid) != 0 ||
        field_after(buf, " gid:", &proc_gid) != 0) {
        printf("pouch-hello-identity: FAIL -- no principal:/gid: in %s\n", path);
        return 1;
    }
    printf("pouch-hello-identity: /proc says principal:%lu gid:%lu\n",
           proc_uid, proc_gid);

    unsigned long ru = (unsigned long)getuid(),  eu = (unsigned long)geteuid();
    unsigned long rg = (unsigned long)getgid(),  eg = (unsigned long)getegid();

    // The named defect, checked before the comparison so a regression says
    // WHICH bug returned rather than just "mismatch".
    if (ru == ENOSYS_SENTINEL || eu == ENOSYS_SENTINEL ||
        rg == ENOSYS_SENTINEL || eg == ENOSYS_SENTINEL) {
        printf("pouch-hello-identity: FAIL -- the 0xFFFFFFDA ENOSYS sentinel is "
               "back (uid=%lu euid=%lu gid=%lu egid=%lu); patch 0043 is not "
               "applied or was reverted\n", ru, eu, rg, eg);
        return 1;
    }

    if (ru != proc_uid || eu != proc_uid) {
        printf("pouch-hello-identity: FAIL -- getuid/geteuid %lu/%lu disagree "
               "with /proc principal:%lu\n", ru, eu, proc_uid);
        return 1;
    }
    printf("pouch-hello-identity: getuid/geteuid -> %lu/%lu (ok, agrees with "
           "/proc)\n", ru, eu);

    if (rg != proc_gid || eg != proc_gid) {
        printf("pouch-hello-identity: FAIL -- getgid/getegid %lu/%lu disagree "
               "with /proc gid:%lu\n", rg, eg, proc_gid);
        return 1;
    }
    printf("pouch-hello-identity: getgid/getegid -> %lu/%lu (ok, agrees with "
           "/proc)\n", rg, eg);

    printf("pouch-hello-identity: not the 0xFFFFFFDA ENOSYS sentinel (ok)\n");

    // The census is what joey matches: a stale binary prints the old marker,
    // so a bake trap that skips the populate cannot pass a probe whose legs it
    // never ran. Five legs, all of them reached to get here.
    puts(POUCH_CENSUS_IDENTITY);
    return 0;
}
