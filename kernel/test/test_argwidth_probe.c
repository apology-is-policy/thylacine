// syscall.argwidth_probe -- ERRORS.md principle 6 reached from EL0: an argument
// that does not fit its declared width is refused, never truncated.
//
// /bin/argwidth-probe makes each covered call twice, one variable apart (the
// argument as its binding passes it, then with a bit set above that width), and
// exits ARGWIDTH_PROBE_STATUS only when every wide call answered the refusal and
// every narrow one answered anything else.
#include "test.h"
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include "../../arch/arm64/uart.h"

void test_syscall_argwidth_probe(void);

#define ARGWIDTH_PROBE_STATUS 72

// The probe names these by number (libt has no wrapper for them).
_Static_assert(SYS_POSTNOTE == 47, "argwidth-probe's SYS_POSTNOTE_NR");
_Static_assert(SYS_BURROW_MAP_FILE == 126, "argwidth-probe's SYS_BURROW_MAP_FILE_NR");

#define ARGWIDTH_PROBE_BLOB_MAX 65536
static _Alignas(16) u8 g_argwidth_probe_blob[ARGWIDTH_PROBE_BLOB_MAX];

struct argwidth_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void argwidth_probe_exec_thunk(void *arg) {
    struct argwidth_probe_exec_args *ea = (struct argwidth_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t || !t->proc) extinction("argwidth_probe_exec_thunk: no current proc");
    u64 entry = 0, sp = 0;
    int rc = exec_setup(t->proc, ea->blob, ea->size, &entry, &sp);
    if (rc != 0) {
        uart_puts("    argwidth-probe exec_setup rc=");
        uart_putdec((u64)rc);
        uart_puts("\n");
        exits("fail-exec");
    }
    userland_enter(entry, sp);
}

void test_syscall_argwidth_probe(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;
    if (test_probe_lookup("bin/argwidth-probe", &cpio_blob, &size) != 0) return;
    TEST_ASSERT(size <= ARGWIDTH_PROBE_BLOB_MAX, "argwidth-probe too large for its buffer");
    // cpio is 4-aligned; the ELF header cast needs 8.
    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_argwidth_probe_blob[i] = src[i];

    struct argwidth_probe_exec_args args = { .blob = g_argwidth_probe_blob, .size = size };
    int pid = rfork(RFPROC, argwidth_probe_exec_thunk, &args);
    TEST_ASSERT(pid > 0, "rfork failed for /argwidth-probe");
    int status = -42;
    int reaped = wait_pid(&status);

    uart_puts("    argwidth-probe status=");
    uart_putdec((u64)status);
    uart_puts(" expected=");
    uart_putdec((u64)ARGWIDTH_PROBE_STATUS);
    uart_puts("\n");
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, ARGWIDTH_PROBE_STATUS,
        "every too-wide argument the probe passed was refused");
}
