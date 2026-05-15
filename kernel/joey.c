// /joey — first userspace process; the long-running init.
//
// At P5-joey-from-ramfs (this chunk), /joey is loaded from the initrd
// cpio via devramfs_lookup instead of an embedded 9-instruction blob in
// the kernel image. The orchestration is unchanged: kernel rforks a
// child, exec_setup's the joey ELF, userland_enter into EL0, wait_pid
// for completion. What changed is the source of the ELF — and therefore
// what /joey can do: an arbitrary userspace binary instead of a
// hand-encoded SVC SYS_PUTS+SYS_EXITS sequence.
//
// The blob is owned by the devramfs file table for the kernel's
// lifetime; exec_setup copies what it needs into the child's address
// space, so the pointer's lifetime is comfortably longer than the
// child Proc's reference to it.
//
// Boot flow:
//   boot_main() ... all bring-up ...
//     test_run_all()                       in-kernel tests (kproc context)
//     fault_test_run()                     hardening proof
//     joey_run()                           ← P5-joey-from-ramfs
//       devramfs_lookup("joey")            obtain ELF bytes from initrd
//       rfork(RFPROC, joey_thunk, args)    child Proc on kthread kstack
//         joey_thunk:
//           exec_setup(p, blob, size)      populate child's address space
//           userland_enter(entry, sp)      eret to EL0 (never returns)
//         child user code runs:
//           t_putstr(...)                  → SYS_PUTS to UART
//           main returns 0 → _start        → SVC SYS_EXITS
//       wait_pid(&status)                  block; reap child
//     "Thylacine boot OK"                  TOOLING.md §10 ABI
//
// At later chunks /joey becomes the long-running supervisor (per
// ARCHITECTURE.md §5.1 + CORVUS-DESIGN.md §3 D4): forks stratumd-system,
// mounts /sysroot via 9P, pivots root, starts /sbin/corvus and login.
// joey_run keeps its current shape (one-call, wait-and-extinct) only
// until the orchestrator extension lands.
//
// Spec posture: no new TLA+ at P5-joey-from-ramfs. The change is the
// source of the ELF, not the orchestration shape; the prior invariants
// (rfork → exec_setup → userland_enter → wait_pid) hold unchanged.

#include <thylacine/joey.h>

#include <thylacine/devramfs.h>
#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"

// File name in the initrd cpio. Built by usr/joey/CMakeLists.txt and
// copied into the cpio root by tools/build.sh::build_ramfs.
#define JOEY_RAMFS_NAME "joey"

// Cpio newc data is only 4-byte-aligned, but the ELF Ehdr cast in
// elf_load (kernel/elf.c::elf_load — R5-G F61) requires 8-byte
// alignment. Copy into an 8-aligned static buffer before handing the
// blob to exec_setup. Sized to match the userspace static-PIE binary
// budget (each binary fits in 16 KiB with headroom; see P4-image-shrink).
#define JOEY_BLOB_MAX 32768
static _Alignas(struct Elf64_Ehdr) u8 g_joey_elf_blob[JOEY_BLOB_MAX];

// Arguments passed via rfork's `arg` to the child entry. Lives on the
// caller (boot CPU) stack for the duration of joey_run(); the child
// reads it once before transitioning to EL0, after which the parent
// blocks in wait_pid().
struct joey_args {
    const void *blob;
    size_t      blob_size;
};

// Child entry. Runs in EL1 on the rfork'd kthread's kstack, in the
// child Proc's context (current_thread()->proc is the new Proc).
// Calls exec_setup + userland_enter; never returns from userland_enter
// (transitions to EL0). On exec_setup failure, exits("fail-exec") so
// the parent's wait_pid observes a non-zero exit_status.
__attribute__((noreturn))
static void joey_thunk(void *arg) {
    struct joey_args *ia = (struct joey_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("joey_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("joey_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ia->blob, ia->blob_size, &entry, &sp);
    if (rc != 0) {
        uart_puts("  joey: exec_setup failed rc=");
        uart_putdec((u64)rc);
        uart_puts("; exits(fail-exec)\n");
        exits("fail-exec");
    }

    userland_enter(entry, sp);
    // userland_enter is __noreturn; control transfers to EL0 atomically.
}

void joey_run(void) {
    // One-call guard. v1.0 invariant — joey_run is called exactly once
    // per boot from boot_main. The guard catches accidental double-call
    // (e.g., a future supervisor refactor that tries to re-run joey_run
    // instead of re-execing).
    static bool g_joey_run_called = false;
    if (g_joey_run_called) {
        extinction("joey_run: double call (v1.0 single-use invariant)");
    }
    g_joey_run_called = true;

    const void *cpio_blob = NULL;
    size_t blob_size = 0;
    if (devramfs_lookup(JOEY_RAMFS_NAME, &cpio_blob, &blob_size) != 0) {
        // Missing /joey is unrecoverable at v1.0: boot path requires
        // it. Surfaces as EXTINCTION: in the boot log so tools/test.sh
        // reports failure.
        extinction("joey: /joey not found in initrd (devramfs_lookup failed)");
    }
    if (blob_size == 0) {
        extinction("joey: /joey in initrd has zero size");
    }
    if (blob_size > JOEY_BLOB_MAX) {
        extinction_with_addr("joey: /joey ELF exceeds JOEY_BLOB_MAX", (u64)blob_size);
    }

    // Copy cpio's 4-aligned bytes into the 8-aligned static buffer the
    // ELF loader requires.
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < blob_size; i++) g_joey_elf_blob[i] = src[i];

    uart_puts("  joey: rforking child for /joey (");
    uart_putdec((u64)blob_size);
    uart_puts(" byte ELF from initrd)\n");

    struct joey_args args = {
        .blob      = g_joey_elf_blob,
        .blob_size = blob_size,
    };

    // P5-corvus-bringup-a: joey is init; it needs to grant
    // CAP_LOCK_PAGES + CAP_CSPRNG_READ to /sbin/corvus (and similarly
    // delegated caps to future child Procs). Plain rfork() would give
    // joey CAP_NONE; spawn-with-caps's AND would then return 0 for
    // every cap bit. rfork_with_caps(CAP_ALL) gives joey the full v1.0
    // capability ceiling so it can grant any subset to children. This
    // matches the production-init pattern: joey is the trusted
    // delegate that distributes caps per child's role.
    int pid = rfork_with_caps(RFPROC, joey_thunk, &args, CAP_ALL);
    if (pid < 0) {
        extinction("joey: rfork_with_caps(RFPROC, joey_thunk) failed");
    }

    int status = -42;
    int reaped = wait_pid(&status);
    if (reaped != pid) {
        extinction_with_addr("joey: wait_pid returned wrong pid", (u64)reaped);
    }
    if (status != 0) {
        extinction_with_addr("joey: /joey exited non-zero", (u64)status);
    }

    uart_puts("  joey: /joey pid=");
    uart_putdec((u64)pid);
    uart_puts(" exited cleanly (status=0)\n");
}
