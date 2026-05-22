// /pouch-hello-stdio — the buffered-stdio pouch hello (Phase 6 sub-chunk 5).
//
// Companion to /pouch-hello: where that binary exercises the raw write(2)
// seam, this one exercises musl's buffered stdio. puts() and fwrite()
// copy bytes into stdout's static FILE buffer; the exit-time flush drains
// the buffer through the patched __stdio_write backend
// (0002-pouch-stdio-no-iovec.patch — a SYS_write loop, since Thylacine
// has no writev).
//
// stdout is *fully* buffered: pouch's tty probe (the first stdio write
// issues SYS_ioctl) hits an unimplemented-syscall sentinel, so musl sees
// "not a tty" and does not line-buffer — the buffer drains at exit, not
// at the newline. The P6-pouch-syscall-seam audit's F2 behaviour,
// observed live.
//
// This is deliberately NOT printf(3). printf pulls musl's vfprintf,
// whose long-double (aarch64 binary128) formatting needs compiler-rt
// soft-float builtins (__eqtf2, __extenddftf2, ...). The pouch sysroot
// has no compiler runtime yet — that is owed to a follow-on
// `pouch-compiler-rt` sub-chunk (see docs/phase6-status.md). The
// buffered-stdio *path* this binary proves is identical whether the
// bytes arrive via puts() or printf().
//
// fd 1 is a pipe write-end joey relays to the boot-log UART and
// content-checks. Cross-compiled with tools/pouch-clang against the
// pouch sysroot.

#include <stdio.h>
#include <string.h>

int main(void) {
    if (puts("pouch-hello-stdio: hello via buffered stdio (puts)") == EOF)
        return 1;

    static const char line[] = "pouch-hello-stdio: a second line via fwrite\n";
    if (fwrite(line, 1, strlen(line), stdout) != strlen(line))
        return 1;

    if (puts("pouch-hello-stdio: buffer drains at exit") == EOF)
        return 1;
    return 0;
}
