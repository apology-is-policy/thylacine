// /hello-from-ramfs regression test (P4-Ia1).
//
// Loads /hello from devramfs (the cpio bound at boot via -initrd) and
// runs it via the existing exec_setup + userland_enter chain. Verifies
// the full build-from-disk pipeline:
//
//   tools/build.sh userspace  → build/usr/hello/hello (static-PIE ELF)
//   tools/build.sh kernel     → build_ramfs copies hello into build/ramfs-src
//   tools/mkcpio.py           → build/ramfs.cpio
//   tools/run-vm.sh           → QEMU -initrd
//   kernel boot               → devramfs parses cpio → g_ramfs_files
//   this test                 → devramfs_lookup("hello", ...) → rfork →
//                               exec_setup → userland_enter → /hello main →
//                               t_putstr → SYS_PUTS → t_exits(0) →
//                               SYS_EXITS → wait_pid reaps
//
// If build/usr/hello/hello wasn't built (user invoked tools/build.sh
// kernel directly without going through tools/build.sh all and userspace
// hasn't been built yet — possible in fresh checkouts), devramfs_lookup
// returns -1; the test prints a skip notice and returns PASS. This keeps
// the kernel test suite green for the bootstrap-only configuration.
//
// Production / CI flow always goes through tools/build.sh all (or
// build_kernel which itself invokes build_userspace), so the binary
// is present and the test exercises the full path.

#include "test.h"

#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_userspace_ramfs_hello(void);
void test_userspace_ramfs_hello_rs(void);

// The cpio newc format aligns data to 4 bytes; the kernel ELF loader
// requires 8-byte alignment for the Ehdr cast (UBSan -fsanitize=alignment
// would otherwise trap; see R5-G F61). Copy into an 8-aligned static
// buffer before handing the blob to exec_setup. P4-Ic7 shrunk 256 → 128
// KiB so cumulative kernel-image .bss + firmware reserve fits the 2 MiB
// L3 mapping (arch/arm64/mmu.c::mmu_map_kernel). 128 KiB still covers
// every userspace binary in usr/* by ~50 KiB.
#define RAMFS_EXEC_BLOB_MAX 131072
static _Alignas(16) u8 g_ramfs_blob[RAMFS_EXEC_BLOB_MAX];

struct ramfs_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void ramfs_exec_thunk(void *arg) {
    struct ramfs_exec_args *ea = (struct ramfs_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("ramfs_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("ramfs_exec_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts(" → exits(fail-exec)\n");
        exits("fail-exec");
    }

    uart_puts("    exec_setup ok entry=");
    uart_puthex64(entry);
    uart_puts(" sp=");
    uart_puthex64(sp);
    uart_puts(" → userland_enter\n");

    userland_enter(entry, sp);
}

// Shared body: load `name` from devramfs, exec it, verify clean exit.
// Returns 0 on success, -1 on TEST_ASSERT/EXPECT failure (caller has
// already called test_fail via the macros), 1 on "not in ramfs" (skip).
static int run_ramfs_binary(const char *name) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup(name, &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /");
        uart_puts(name);
        uart_puts(" not in ramfs (build with: tools/build.sh all)\n");
        return 1;
    }

    if (size > RAMFS_EXEC_BLOB_MAX) {
        test_fail("ramfs binary too large for static buffer");
        return -1;
    }

    // Copy into 8-aligned static buffer (cpio data is only 4-aligned;
    // ELF Ehdr cast requires 8 — UBSan -fsanitize=alignment / R5-G F61).
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_ramfs_blob[i] = src[i];

    uart_puts("    /");
    uart_puts(name);
    uart_puts(" cpio=");
    uart_puthex64((u64)(uintptr_t)cpio_blob);
    uart_puts(" → aligned=");
    uart_puthex64((u64)(uintptr_t)g_ramfs_blob);
    uart_puts(" size=");
    uart_putdec((u64)size);
    uart_puts(" bytes\n");

    struct ramfs_exec_args args = { .blob = g_ramfs_blob, .size = size };

    int pid = rfork(RFPROC, ramfs_exec_thunk, &args);
    if (pid <= 0) {
        test_fail("rfork failed for ramfs binary");
        return -1;
    }

    int status = -42;
    int reaped = wait_pid(&status);
    if (reaped != pid) {
        test_fail("wait_pid returned wrong pid for ramfs binary");
        return -1;
    }
    if (status != 0) {
        test_fail("ramfs binary exited non-zero");
        return -1;
    }

    uart_puts("    /");
    uart_puts(name);
    uart_puts(" reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" (build path verified)\n");
    return 0;
}

// /hello — C-side runtime (libt) + first hello binary (P4-Ia1).
void test_userspace_ramfs_hello(void) {
    run_ramfs_binary("hello");
}

// /hello-rs — Rust nostd userspace + first Rust hello binary (P4-Ia2).
// Verifies the cargo + rust-lld + global_asm! _start path through the
// same exec_setup pipeline. Different binary, same kernel-side machinery.
void test_userspace_ramfs_hello_rs(void) {
    run_ramfs_binary("hello-rs");
}
