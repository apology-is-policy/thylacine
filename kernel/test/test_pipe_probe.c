// /pipe-probe regression test (P5-fd-syscalls).
//
// Pipeline:
//   tools/build.sh userspace  → build/usr/pipe-probe/pipe-probe (ELF)
//   build_ramfs                → cpio includes /pipe-probe
//   boot                       → devramfs reads cpio
//   this test                  → devramfs_lookup("pipe-probe") →
//                                rfork → exec_setup → userland_enter
//                                → main → t_pipe + t_read + t_write +
//                                  t_dup + t_close + t_putstr("PASS")
//                                  + t_exits(0)
//   wait_pid                   → status = 0 → test PASS
//
// If /pipe-probe wasn't built (user invoked tools/build.sh kernel only)
// the devramfs lookup fails — we SKIP rather than FAIL so the kernel
// boot test runner still passes.

#include "test.h"

#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"

void test_pipe_probe_round_trip(void);

// pipe-probe is slightly larger than virtio-blk-probe etc. (12-16 KiB
// range): the inline syscall stubs for pipe/read/write/close/dup
// each instantiate per-call SVC sequences. 32 KiB cap with headroom.
#define PIPE_PROBE_BLOB_MAX 32768
static _Alignas(16) u8 g_pipe_probe_blob[PIPE_PROBE_BLOB_MAX];

struct pipe_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void pipe_probe_exec_thunk(void *arg) {
    struct pipe_probe_exec_args *ea = (struct pipe_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("pipe_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("pipe_probe_exec_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    pipe-probe exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts(" → exits(fail-exec)\n");
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

void test_pipe_probe_round_trip(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("pipe-probe", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /pipe-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    TEST_ASSERT(size <= PIPE_PROBE_BLOB_MAX,
                "pipe-probe binary too large for static buffer");

    // Copy into 8-aligned static buffer (cpio is 4-aligned; ELF Ehdr
    // cast requires 8 — R5-G F61).
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_pipe_probe_blob[i] = src[i];

    uart_puts("    /pipe-probe size=");
    uart_putdec((u64)size);
    uart_puts(" bytes → rfork + exec\n");

    struct pipe_probe_exec_args args = {
        .blob = g_pipe_probe_blob, .size = size
    };

    int pid = rfork(RFPROC, pipe_probe_exec_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork failed for /pipe-probe");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0,
        "/pipe-probe exit status (0 = PASS: pipe + read/write + dup + close round-trip)");

    uart_puts("    /pipe-probe reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — full byte-I/O syscall surface verified end-to-end\n");
}
