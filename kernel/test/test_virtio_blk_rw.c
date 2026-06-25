// /virtio-blk-rw regression test (P4-Ic7).
//
// Closes ROADMAP §6.2 exit criterion: "Userspace virtio-blk: read 1 GiB
// from VirtIO block device successfully; write 1 GiB and read it back,
// verify bit-exact."
//
// Successor to test_virtio_blk_probe (P4-Ic5b2). The probe verifies
// the basic VirtIO 1.2 init + a SINGLE 512-byte read of sector 0's
// signature. This test exercises the multi-sector + write paths with
// a deterministic pattern over a substantially larger range of the
// disk:
//
//   - Pass A: VIRTIO_BLK_T_IN over sectors [1..N+1) (in 2048-sector
//             requests = 1 MiB each). Userspace verifies each sector's
//             contents against the LCG-A pattern that tools/mkdisk.py
//             baked into the disk image at build time.
//
//   - Pass B: VIRTIO_BLK_T_OUT over the same range. Userspace fills
//             the data buffer with the LCG-B pattern (distinct from A
//             via a different multiplier + a seed-XOR) and submits the
//             write.
//
//   - Pass C: VIRTIO_BLK_T_IN over the same range; verify each sector
//             matches the LCG-B pattern. Confirms the device persisted
//             our writes byte-exactly.
//
// Per-pass N is computed by the userspace binary from the VirtIO
// Capacity field at config offset 0, capped at 1 GiB. Default
// THYLACINE_DISK_SIZE=16M produces N = 15 MiB per pass (16 MiB - 1
// MiB-round-down for the unused tail = 15 MiB of granular 1-MiB
// batches). THYLACINE_DISK_SIZE=1G enables the full 1 GiB exit run.
//
// Kernel-side responsibilities:
//   - Look up /virtio-blk-rw in devramfs; copy to the static blob
//     (with R5-G alignment).
//   - Confirm a virtio-mmio slot with DeviceID = 2 (block) exists
//     (graceful SKIP if -device virtio-blk-device is absent in
//     tools/run-vm.sh's flags, e.g. tools/run-vm.sh --no-share for a
//     headless dev VM that nonetheless ships disk-less).
//   - rfork_with_caps(RFPROC, ..., CAP_HW_CREATE).
//   - wait_pid; assert exit_status == 0.
//
// Static blob: 128 KiB (same as virtio-blk-probe). virtio-blk-rw
// compiles to ~74 KiB; 128 KiB gives growth headroom while staying
// under the cumulative-.bss envelope QEMU uses to compute the
// -initrd placement on the ARM virt machine. A 256 KiB blob (tried
// first) pushed the kernel image past that envelope and QEMU placed
// the initrd into our .bss range — the kernel never reached its boot
// banner.

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

void test_virtio_blk_rw_rfork_with_caps(void);

// 16 KiB per blob (P4-image-shrink convention; every userspace binary
// fits under 16 KiB with -z max-page-size=4096 on both Rust + C sides).
#define VIRTIO_BLK_RW_BLOB_MAX 32768
static _Alignas(16) u8 g_virtio_blk_rw_blob[VIRTIO_BLK_RW_BLOB_MAX];

struct virtio_blk_rw_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void virtio_blk_rw_exec_thunk(void *arg) {
    struct virtio_blk_rw_exec_args *ea =
        (struct virtio_blk_rw_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("virtio_blk_rw_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("virtio_blk_rw_exec_thunk: no proc");

    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    virtio_blk_rw_exec_thunk: child lacks CAP_HW_CREATE\n");
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

void test_virtio_blk_rw_rfork_with_caps(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("virtio-blk-rw", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /virtio-blk-rw not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    // Same SKIP logic as virtio-blk-probe: if no virtio-blk-device is
    // wired in QEMU, the userspace probe would scan + bail early but
    // we short-circuit kernel-side to keep the test runner output
    // clean.
    struct virtio_mmio_dev *blk_dev =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_BLOCK);
    if (!blk_dev) {
        uart_puts("    [skip] no virtio-mmio slot with DeviceID=2\n");
        return;
    }

    TEST_ASSERT(size <= VIRTIO_BLK_RW_BLOB_MAX,
                "virtio-blk-rw binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_virtio_blk_rw_blob[i] = src[i];

    uart_puts("    /virtio-blk-rw size=");
    uart_putdec((u64)size);
    uart_puts(" bytes; blk_dev pa=");
    uart_puthex64((u64)blk_dev->pa);
    uart_puts(" → rfork_with_caps(CAP_HW_CREATE)\n");

    struct virtio_blk_rw_exec_args args = {
        .blob = g_virtio_blk_rw_blob, .size = size
    };

    int pid = rfork_with_caps(RFPROC, virtio_blk_rw_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /virtio-blk-rw");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "/virtio-blk-rw exit status (0 = three-pass r/w/readback OK)");

    uart_puts("    /virtio-blk-rw reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — multi-sector r/w verified end-to-end\n");
}
