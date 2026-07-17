// /pouch-hello — the first POSIX C program Thylacine runs (Phase 6 sub-chunk 5).
//
// Cross-compiled with tools/pouch-clang against the pouch sysroot
// (build/sysroot): musl's static CRT + libc.a, retargeted at the syscall
// seam to the Thylacine ABI. This binary is the first *runtime* exercise
// of everything sub-chunks 3-4 built:
//   - the System V startup frame (auxv) the kernel writes in exec_setup
//   - musl's static CRT: _start -> __libc_start_main -> __init_libc -> main
//   - SYS_SET_TID_ADDRESS, issued by musl's thread-pointer init
//   - the raw write(2) seam — write maps 1:1 onto SYS_WRITE
//   - the exit seam — exit() folds onto SYS_EXITS
//
// It is also the durable runtime regression for the P6-pouch-syscall-seam
// 0xFFFF unimplemented-syscall sentinel. Until a pouch binary could run,
// the only regression was build_sysroot's structural greps. Two checks,
// one per musl syscall path — both must short-circuit to errno == ENOSYS
// *without* issuing the trap (a foreign number reaching the kernel would
// be rejected with a flat -1, decoding to EIO, not ENOSYS):
//   - chdir() — non-cancellable; SYS_chdir is the sentinel, caught by the
//     __syscall0..6 guard in syscall_arch.h.
//   - open()  — a cancellation point; SYS_openat is the sentinel, caught
//     by the __syscall_cp guard (the P6-pouch-syscall-seam audit's F1
//     fix — the cancellable path had bypassed the non-cancellable guards).
//
// stdout (fd 1) is a pipe write-end joey installs via t_spawn_with_fds;
// joey relays it to the boot-log UART and content-checks it. main returns
// non-zero on any failed assertion — joey treats that as a boot regression.

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// emit — write the whole NUL-terminated string to stdout, looping on
// short writes. Returns 0 on success, -1 if a write makes no progress.
static int emit(const char *s) {
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(1, s + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int main(void) {
    if (emit("pouch-hello: hello from a POSIX C program (pouch libc)\n") != 0)
        return 1;

    // The path argument is never resolved — each syscall short-circuits
    // in pouch before the trap — so it is deliberately bogus.
    static const char probe_path[] = "/pouch-seam-probe-never-resolved";

    errno = 0;
    if (chdir(probe_path) != -1 || errno != ENOSYS) {
        (void)emit("pouch-hello: FAIL non-cancellable sentinel (chdir)\n");
        return 1;
    }
    if (emit("pouch-hello: sentinel ok - chdir -> ENOSYS (non-cancellable path)\n") != 0)
        return 1;

    // P6-pouch-stratumd-boot 16b-gamma: open() now works (it forwards to
    // openat -> SYS_walk_open via patches 0009 + 0010). The cancellable
    // sentinel test is repurposed: a non-existent path returns ENOENT
    // (the file walk misses), not ENOSYS. The cancellable-path syscall
    // surface that DOES still 0xFFFF is exercised elsewhere.
    errno = 0;
    if (open(probe_path, O_RDONLY) != -1 || errno != ENOENT) {
        (void)emit("pouch-hello: FAIL open returns ENOENT for missing path\n");
        return 1;
    }
    if (emit("pouch-hello: open -> ENOENT (16b-gamma openat live)\n") != 0)
        return 1;

    // #37: pread/pwrite ride SYS_PREAD/SYS_PWRITE (musl's pread64/pwrite64
    // seam numbers went 0xFFFF -> 85/86). Pre-#37 both short-circuited to
    // ENOSYS, so a successful pread IS the wiring proof. /welcome is the
    // read-only devramfs smoke file ("Welcome to Thylacine ramfs.\n",
    // pinned by tools/build.sh) -- runs PRE-pivot like the stdio prover's
    // /version read. The read()-pread()-read() sandwich proves the POSIX
    // cursor contract end-to-end through musl: the positioned read must
    // not move the fd cursor.
    {
        int fd = open("/welcome", O_RDONLY);
        if (fd < 0) {
            (void)emit("pouch-hello: FAIL open /welcome\n");
            return 1;
        }
        char a[4], b[4], c[3];
        if (read(fd, a, 4) != 4 || memcmp(a, "Welc", 4) != 0) {
            (void)emit("pouch-hello: FAIL read /welcome head\n");
            return 1;
        }
        errno = 0;
        ssize_t pn = pread(fd, b, 4, 1);
        if (pn != 4 || memcmp(b, "elco", 4) != 0) {
            (void)emit(errno == ENOSYS
                       ? "pouch-hello: FAIL pread -> ENOSYS (seam not wired)\n"
                       : "pouch-hello: FAIL pread @1 content\n");
            return 1;
        }
        if (read(fd, c, 3) != 3 || memcmp(c, "ome", 3) != 0) {
            (void)emit("pouch-hello: FAIL cursor moved by pread\n");
            return 1;
        }
        // pwrite must reach the kernel (devramfs is read-only and the fd is
        // O_RDONLY, so it fails there) -- ENOSYS would mean the seam number
        // never left musl.
        errno = 0;
        if (pwrite(fd, "x", 1, 0) != -1 || errno == ENOSYS) {
            (void)emit("pouch-hello: FAIL pwrite seam (expected kernel -1, not ENOSYS)\n");
            return 1;
        }
        (void)close(fd);
        if (emit("pouch-hello: pread/pwrite seam ok (#37: cursor untouched, wired to 85/86)\n") != 0)
            return 1;
    }

    // #33: sched_yield rides SYS_YIELD (musl's seam number went 0xFFFF -> 87).
    // Pre-#33 it short-circuited to ENOSYS -> musl returned -1, so rc == 0 IS
    // the wiring proof (the kernel handler always returns 0).
    errno = 0;
    if (sched_yield() != 0) {
        (void)emit(errno == ENOSYS
                   ? "pouch-hello: FAIL sched_yield -> ENOSYS (seam not wired)\n"
                   : "pouch-hello: FAIL sched_yield rc\n");
        return 1;
    }
    if (emit("pouch-hello: sched_yield ok (#33: wired to 87)\n") != 0)
        return 1;

    // POUNCE P-4 (patch 0019): stat(path) rides SYS_STAT (musl's fstatat
    // AT_FDCWD/absolute forms went 0xFFFF -> SYS_thyla_stat 88; there is
    // no aarch64 __NR_stat, so pre-0019 the whole stat(path) family
    // short-circuited to ENOSYS -- success IS the wiring proof). Field
    // equality against fstat(2) on the same file proves the 0019 t_stat
    // translation agrees with 0010's (both mirror the 80-byte kernel
    // struct); the miss leg proves resolution errnos pass through.
    {
        struct stat st, fst;
        errno = 0;
        if (stat("/welcome", &st) != 0) {
            (void)emit(errno == ENOSYS
                       ? "pouch-hello: FAIL stat -> ENOSYS (0019 seam not wired)\n"
                       : "pouch-hello: FAIL stat /welcome\n");
            return 1;
        }
        int fd = open("/welcome", O_RDONLY);
        if (fd < 0 || fstat(fd, &fst) != 0) {
            (void)emit("pouch-hello: FAIL fstat /welcome (stat probe)\n");
            return 1;
        }
        (void)close(fd);
        if (st.st_size != fst.st_size || st.st_ino != fst.st_ino ||
            st.st_mode != fst.st_mode || st.st_uid != fst.st_uid ||
            st.st_gid != fst.st_gid) {
            (void)emit("pouch-hello: FAIL stat/fstat field mismatch\n");
            return 1;
        }
        errno = 0;
        if (stat("/no-such-pouch-stat-probe", &st) != -1 || errno != ENOENT) {
            (void)emit("pouch-hello: FAIL stat miss -> ENOENT\n");
            return 1;
        }
        if (emit("pouch-hello: stat(path) ok (0019: SYS_STAT=88, fields == fstat, miss -> ENOENT)\n") != 0)
            return 1;
    }

    // clock_gettime rides SYS_CLOCK_GETTIME (musl's seam number went
    // 0xFFFF -> 75; the LS-K kernel surface predates the wiring). Pre-wiring
    // it short-circuited to ENOSYS and left the timespec untouched, so a
    // 0-rc + a sane advancing MONOTONIC value IS the wiring proof. musl
    // routes gettimeofday()/time() through the same call, so this covers
    // the whole ported-code time family.
    {
        struct timespec t1, t2;
        errno = 0;
        if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
            (void)emit(errno == ENOSYS
                       ? "pouch-hello: FAIL clock_gettime -> ENOSYS (seam not wired)\n"
                       : "pouch-hello: FAIL clock_gettime rc\n");
            return 1;
        }
        for (volatile int spin = 0; spin < 50000; spin++) { }
        if (clock_gettime(CLOCK_MONOTONIC, &t2) != 0) {
            (void)emit("pouch-hello: FAIL clock_gettime 2nd rc\n");
            return 1;
        }
        if (t1.tv_sec == 0 && t1.tv_nsec == 0) {
            (void)emit("pouch-hello: FAIL clock_gettime returned zero time\n");
            return 1;
        }
        if (t2.tv_sec < t1.tv_sec ||
            (t2.tv_sec == t1.tv_sec && t2.tv_nsec < t1.tv_nsec)) {
            (void)emit("pouch-hello: FAIL MONOTONIC went backward\n");
            return 1;
        }
        errno = 0;
        if (clock_gettime(CLOCK_REALTIME, &t1) != 0 || t1.tv_sec < 1577836800) {
            // 2020-01-01 floor: the LS-K RTC anchor guarantees a post-2020
            // wall clock on QEMU-virt (PL031 present).
            (void)emit("pouch-hello: FAIL clock_gettime REALTIME\n");
            return 1;
        }
        if (emit("pouch-hello: clock_gettime ok (wired to 75: MONOTONIC advances, REALTIME > 2020)\n") != 0)
            return 1;
    }

    if (emit("pouch-hello: exit 0\n") != 0)
        return 1;
    return 0;
}
