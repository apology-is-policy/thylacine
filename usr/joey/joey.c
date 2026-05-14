// /joey — first userspace process; the long-running init.
//
// At P5-joey-from-ramfs (this chunk), /joey is a tiny userspace binary
// that replaces the prior kernel-embedded 9-instruction blob. The kernel
// drives the bring-up handshake the same way (kernel/joey.c: devramfs
// lookup, rfork, exec_setup, wait_pid); what changed is the source of
// the ELF — from compile-time-embedded to ramfs-loaded.
//
// This is the predecessor shape for P5-stratumd-bringup, where /joey
// extends to orchestrate stratumd-system, attach the system pool's 9P
// tree, and pivot the boot CPU's territory root. The minimum viable
// version here exits 0 so the existing boot-banner contract (TOOLING.md
// §10) stays intact.

#include <thyla/syscall.h>

int main(void) {
    t_putstr("joey: hello from /joey (real userspace binary, loaded from ramfs)\n");
    return 0;
}
