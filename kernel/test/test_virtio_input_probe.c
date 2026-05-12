// /virtio-input regression test (P4-K).
//
// Third composed-driver test. Symmetric to test_virtio_{blk,net}_probe.c:
// spawns a userspace child with CAP_HW_CREATE; child claims its
// virtio-mmio slot via SYS_MMIO_CREATE + SYS_MMIO_MAP; allocates a DMA
// buffer via SYS_DMA_CREATE + SYS_DMA_MAP; runs VirtIO 1.2 init for
// the INPUT device class; reads device name + EV_BITS via the
// selector-based config-space; reaches DRIVER_OK; exits 0.
//
// What this test specifically guards against (beyond what blk+net
// probes cover):
//   - DeviceID dispatch for ID=18 in find_by_device_id.
//   - Selector-based config-space access at offset 0x100: writing
//     select (0) + subsel (1), reading size byte (2), reading the
//     union starting at offset 8.
//   - eventq (queue 0) configuration as RX-direction: 16 descriptors
//     pre-published with VIRTQ_DESC_F_WRITE; avail.idx=16 BEFORE
//     DRIVER_OK so the device has all buffers from the moment events
//     would become legal.
//   - Per-queue Queue_DESC/DRIVER/DEVICE bus addresses for queue index
//     0 (virtio-blk uses queue 0; virtio-net uses queue 1; virtio-input
//     uses queue 0 — same queue index as blk but RX-direction this
//     time, which crosses the "tail end" of the kobj_mmio + virtqueue
//     code paths).
//
// SKIP behavior: if /virtio-input is absent from the ramfs (the
// userspace crate wasn't built) OR no virtio-mmio slot reports
// DeviceID=18 (run-vm.sh missing -device virtio-keyboard-device, or
// THYLACINE_NO_INPUT=1 set), short-circuit cleanly.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/devramfs.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>

#include "../../arch/arm64/uart.h"

void test_virtio_input_probe_rfork_with_caps(void);

// 16 KiB blob cap. P4-K landed `-z max-page-size=4096` in
// usr/.cargo/config.toml, which closed rust-lld's 64-KiB
// MAXPAGESIZE-driven file gap; the virtio-input release ELF now lives
// at ~11 KiB (versus ~72 KiB pre-flag). 16 KiB gives ~5 KiB headroom
// per probe + keeps the kernel-side .bss reservation modest. Earlier
// composed-driver binaries (virtio-blk / virtio-net) keep their
// pre-P4-K 96-KiB caps; tightening those is a separate sub-chunk.
#define VIRTIO_INPUT_PROBE_BLOB_MAX 16384
static _Alignas(16) u8 g_virtio_input_probe_blob[VIRTIO_INPUT_PROBE_BLOB_MAX];

struct virtio_input_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void virtio_input_probe_exec_thunk(void *arg) {
    struct virtio_input_probe_exec_args *ea =
        (struct virtio_input_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("virtio_input_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("virtio_input_probe_exec_thunk: no proc");

    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    virtio_input_probe_exec_thunk: child lacks CAP_HW_CREATE\n");
        exits("fail-no-cap");
    }

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
    uart_puts(" caps=0x");
    uart_puthex64(p->caps);
    uart_puts(" → userland_enter\n");

    userland_enter(entry, sp);
}

void test_virtio_input_probe_rfork_with_caps(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("virtio-input", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /virtio-input not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    struct virtio_mmio_dev *input_dev =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_INPUT);
    if (!input_dev) {
        uart_puts("    [skip] no virtio-mmio slot with DeviceID=18 (run-vm.sh missing -device virtio-keyboard-device, or THYLACINE_NO_INPUT=1?)\n");
        return;
    }

    TEST_ASSERT(size <= VIRTIO_INPUT_PROBE_BLOB_MAX,
                "virtio-input binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_virtio_input_probe_blob[i] = src[i];

    uart_puts("    /virtio-input size=");
    uart_putdec((u64)size);
    uart_puts(" bytes; input_dev pa=");
    uart_puthex64((u64)input_dev->pa);
    uart_puts(" → rfork_with_caps(CAP_HW_CREATE)\n");

    struct virtio_input_probe_exec_args args = {
        .blob = g_virtio_input_probe_blob, .size = size
    };

    int pid = rfork_with_caps(RFPROC, virtio_input_probe_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /virtio-input");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "/virtio-input exit status (0 = config-space + eventq init reached DRIVER_OK)");

    uart_puts("    /virtio-input reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — selector-based config-space + eventq RX init end-to-end\n");
}
