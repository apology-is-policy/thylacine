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
#include <string.h>
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

    errno = 0;
    if (open(probe_path, O_RDONLY) != -1 || errno != ENOSYS) {
        (void)emit("pouch-hello: FAIL cancellable sentinel (open)\n");
        return 1;
    }
    if (emit("pouch-hello: sentinel ok - open -> ENOSYS (cancellable path)\n") != 0)
        return 1;

    if (emit("pouch-hello: exit 0\n") != 0)
        return 1;
    return 0;
}
