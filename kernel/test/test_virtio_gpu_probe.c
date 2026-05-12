// /virtio-gpu regression test (P4-L + P4-L-scanout).
//
// Fourth composed-driver test. Symmetric to test_virtio_{blk,net,input}_probe.c:
// spawns a userspace child with CAP_HW_CREATE; child claims its
// virtio-mmio slot via SYS_MMIO_CREATE + SYS_MMIO_MAP; subscribes to
// the slot's IRQ via SYS_IRQ_CREATE; allocates two DMA buffers via
// SYS_DMA_CREATE + SYS_DMA_MAP (4 KiB ring + 64 KiB framebuffer);
// runs VirtIO 1.2 init for the GPU device class (configuring BOTH
// controlq + cursorq); drives the full 2D scanout pipeline via six
// controlq commands; exits 0 iff every command returns its expected
// OK response type.
//
// What this test guards against:
//   - DeviceID dispatch for ID=16 in find_by_device_id.
//   - Two-queue virtio-mmio configuration: REG_QUEUE_SEL=0 → set up
//     controlq; REG_QUEUE_SEL=1 → set up cursorq; both reach
//     QueueReady=1 before DRIVER_OK.
//   - controlq command/response chain: descriptor 0 OUT (req hdr +
//     body) + descriptor 1 IN (resp payload). virtio-gpu folds
//     status into resp.hdr.type (no separate status byte).
//   - Flat le32 config-space at offset 0x100..0x110 (events_read,
//     events_clear, num_scanouts, num_capsets).
//   - Multi-command controlq flow: each command bumps avail.idx by 1
//     and waits its own IRQ; used.idx tracks monotonically; descriptor
//     head 0 is rebuilt + reused between commands.
//   - Two-DMA composition pattern: a small (4 KiB) ring DMA + a
//     larger (64 KiB) framebuffer DMA, each its own KObj_DMA handle
//     mapped at a distinct user-VA window.
//   - Full 2D scanout pipeline (Halcyon-prep gate):
//       GET_DISPLAY_INFO       → OK_DISPLAY_INFO (0x1101)
//       RESOURCE_CREATE_2D     → OK_NODATA (0x1100)
//       RESOURCE_ATTACH_BACKING → OK_NODATA
//       SET_SCANOUT            → OK_NODATA
//       TRANSFER_TO_HOST_2D    → OK_NODATA
//       RESOURCE_FLUSH         → OK_NODATA
//     The six OK responses are a tight contract: QEMU's virtio-gpu
//     device validates resource_id + format + dimensions + backing
//     length + scanout id + rect bounds and answers ERR_INVALID_*
//     on any mismatch.
//
// SKIP behavior: if /virtio-gpu is absent from the ramfs (the
// userspace crate wasn't built) OR no virtio-mmio slot reports
// DeviceID=16 (run-vm.sh missing -device virtio-gpu-device, or
// THYLACINE_NO_GPU=1 set), short-circuit cleanly.

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

void test_virtio_gpu_probe_rfork_with_caps(void);

// 16 KiB blob cap. P4-K landed `-z max-page-size=4096` in
// usr/.cargo/config.toml, which closed rust-lld's 64-KiB
// MAXPAGESIZE-driven file gap; the virtio-gpu release ELF lives at
// ~12 KiB (versus ~73 KiB pre-flag). 16 KiB gives ~4 KiB headroom per
// probe + keeps the kernel-side .bss reservation modest.
#define VIRTIO_GPU_PROBE_BLOB_MAX 16384
static _Alignas(16) u8 g_virtio_gpu_probe_blob[VIRTIO_GPU_PROBE_BLOB_MAX];

struct virtio_gpu_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void virtio_gpu_probe_exec_thunk(void *arg) {
    struct virtio_gpu_probe_exec_args *ea =
        (struct virtio_gpu_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("virtio_gpu_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("virtio_gpu_probe_exec_thunk: no proc");

    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    virtio_gpu_probe_exec_thunk: child lacks CAP_HW_CREATE\n");
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

void test_virtio_gpu_probe_rfork_with_caps(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("virtio-gpu", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /virtio-gpu not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    struct virtio_mmio_dev *gpu_dev =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_GPU);
    if (!gpu_dev) {
        uart_puts("    [skip] no virtio-mmio slot with DeviceID=16 (run-vm.sh missing -device virtio-gpu-device, or THYLACINE_NO_GPU=1?)\n");
        return;
    }

    TEST_ASSERT(size <= VIRTIO_GPU_PROBE_BLOB_MAX,
                "virtio-gpu binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_virtio_gpu_probe_blob[i] = src[i];

    uart_puts("    /virtio-gpu size=");
    uart_putdec((u64)size);
    uart_puts(" bytes; gpu_dev pa=");
    uart_puthex64((u64)gpu_dev->pa);
    uart_puts(" → rfork_with_caps(CAP_HW_CREATE)\n");

    struct virtio_gpu_probe_exec_args args = {
        .blob = g_virtio_gpu_probe_blob, .size = size
    };

    int pid = rfork_with_caps(RFPROC, virtio_gpu_probe_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /virtio-gpu");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "/virtio-gpu exit status (0 = controlq GET_DISPLAY_INFO round-trip OK)");

    uart_puts("    /virtio-gpu reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — full 2D scanout pipeline end-to-end (6 controlq commands)\n");
}
