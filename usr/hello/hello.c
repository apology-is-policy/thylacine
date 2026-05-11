// /hello — first Thylacine userspace binary built from the usr/ tree.
//
// Validates the full chunk-of-tools pipeline: clang (userspace toolchain)
// → libt link → static-PIE ELF → cpio (build_ramfs) → -initrd at boot →
// cpio_newc_iter → devramfs file table → exec_setup (kernel ELF loader)
// → userland_enter → _start → main → t_putstr (SYS_PUTS) → return →
// _start's SVC SYS_EXITS → kernel exits("ok").
//
// Predecessor: kernel/joey.c (the hand-encoded embedded blob in the
// kernel image). /joey continues to run at boot for boot-path liveness;
// /hello demonstrates the build-and-load-from-disk path that all
// subsequent userspace binaries (P4-Ia2 Rust, P4-Ic virtio-blk driver,
// Halcyon, etc.) will use.

#include <thyla/syscall.h>

int main(void) {
    t_putstr("hello from /hello (built via tools/build.sh userspace)\n");
    return 0;
}
