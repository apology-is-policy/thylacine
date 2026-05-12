// /virtio-blk-probe regression test (P4-Ic5b2).
//
// First end-to-end userspace block-driver test. Builds on
// test_mmio_probe.c (P4-Ic5a) and test_irq_probe.c (P4-Ic5-IRQ-probe)
// by composing all four hardware-handle syscalls in a single child
// process:
//   - SYS_MMIO_CREATE + SYS_MMIO_MAP for the virtio-mmio transport.
//   - SYS_DMA_CREATE + SYS_DMA_MAP for the virtqueue rings + req /
//     data / status descriptors.
//   - SYS_IRQ_CREATE + SYS_IRQ_WAIT for the device's completion IRQ.
//
// Setup:
//   1. devramfs_lookup("virtio-blk-probe", ...) — pre-built userspace
//      ELF from the cpio. Graceful skip if not built.
//   2. Verify a virtio-mmio slot with DeviceID = 2 (block) exists in
//      the kernel's probe table (populated by virtio_init at boot).
//      Without a `-device virtio-blk-device,drive=disk0` flag in
//      tools/run-vm.sh's invocation, no slot will have DeviceID=2 and
//      we report SKIP rather than spawning a probe that would block
//      forever on t_irq_wait.
//   3. rfork_with_caps(RFPROC, ..., CAP_HW_CREATE) — child starts
//      with child->caps = kproc.caps & CAP_HW_CREATE = CAP_HW_CREATE.
//
// Child execution:
//   1. exec_setup + userland_enter → _start (libthyla-rs) → rs_main.
//   2. Probe scans virtio-mmio slots 0..31 for DeviceID=2.
//   3. Claims slot via SYS_MMIO_CREATE + SYS_MMIO_MAP (4 pages of
//      the virtio-mmio bank — needed because slots aren't
//      page-aligned).
//   4. Subscribes to slot's IRQ via SYS_IRQ_CREATE.
//   5. Allocates DMA buffer via SYS_DMA_CREATE + SYS_DMA_MAP.
//   6. Runs VirtIO 1.2 init: RESET → ACK → DRIVER → DeviceFeatures →
//      DriverFeatures (VIRTIO_F_VERSION_1) → FEATURES_OK → virtqueue
//      setup → DRIVER_OK.
//   7. Submits a VIRTIO_BLK_T_IN request for sector 0.
//   8. Waits on the IRQ via SYS_IRQ_WAIT.
//   9. Verifies status byte == 0 + data buffer first 16 bytes match
//      "THYLACINE-DISK-1" (the on-disk signature from tools/build.sh
//      disk).
//   10. Exits 0 on success, 1 on any failure.
//
// Test reaps via wait_pid + asserts exit_status == 0.
//
// What this test specifically guards against:
//   - Cumulative regression in the hw-handle syscalls under composition:
//     each one is exercised by an existing probe in isolation, but the
//     composition surfaces interactions (e.g., a future change that
//     made the DMA arm of userland_demand_page use Device-nGnRnE
//     instead of Normal-WB would not be caught by mmio-probe but
//     would manifest here as the device's writes to the data buffer
//     never being CPU-visible — coherency failure).
//   - VirtIO 1.2 init sequence: any deviation from §3.1.1 ordering
//     (e.g., skipping FEATURES_OK readback) causes the device to set
//     STATUS_FAILED and reject DRIVER_OK; the probe reports the
//     specific step that failed.
//   - Split virtqueue semantics: a malformed descriptor chain (wrong
//     flags, wrong next pointer, head pointing into an unmapped page)
//     would cause the device to never complete the request; t_irq_wait
//     blocks until BOOT_TIMEOUT.
//
// If the probe binary OR the virtio-blk device is absent, the test
// prints a SKIP notice and returns PASS — production / CI both ship
// the binary AND the device, so this guards against partial builds
// in development workflows.

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

void test_virtio_blk_probe_rfork_with_caps(void);

// 8-aligned static buffer for the loaded ELF blob (R5-G F61 alignment
// requirement on the Ehdr cast in exec_setup). 128 KiB headroom —
// virtio-blk-probe is small (~16 KiB even with the VirtIO state
// machine + libthyla-rs). 128 KiB is double the current binary; we
// avoid 256 KiB to keep the cumulative .bss across mmio-probe +
// irq-probe + virtio-blk-probe under the image_size envelope QEMU
// uses to compute the -initrd placement on the ARM virt machine.
// P4-Jc shrank further to 96 KiB to keep image+firmware ≤ 2 MiB
// after adding the virtio-net-loop blob.
#define VIRTIO_BLK_PROBE_BLOB_MAX 98304
static _Alignas(16) u8 g_virtio_blk_probe_blob[VIRTIO_BLK_PROBE_BLOB_MAX];

struct virtio_blk_probe_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void virtio_blk_probe_exec_thunk(void *arg) {
    struct virtio_blk_probe_exec_args *ea =
        (struct virtio_blk_probe_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("virtio_blk_probe_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("virtio_blk_probe_exec_thunk: no proc");

    // Diagnostic: confirm CAP_HW_CREATE survived the rfork_with_caps
    // grant. CapsCeiling pins this at the spec layer; this assert
    // catches a regression at the runtime layer.
    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    virtio_blk_probe_exec_thunk: child lacks CAP_HW_CREATE (rfork_with_caps grant lost?)\n");
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

void test_virtio_blk_probe_rfork_with_caps(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("virtio-blk-probe", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /virtio-blk-probe not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    // Verify the kernel-side virtio_init at boot saw a block device.
    // Without `-device virtio-blk-device,drive=disk0` in tools/run-vm.sh,
    // QEMU exposes no virtio-mmio slot with DeviceID=2; the probe
    // would scan 32 slots, find none, exit cleanly with status 0
    // (its own SKIP path). We short-circuit that here so the kernel
    // test runner sees a clean SKIP without spawning a child.
    struct virtio_mmio_dev *blk_dev =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_BLOCK);
    if (!blk_dev) {
        uart_puts("    [skip] no virtio-mmio slot with DeviceID=2 (run-vm.sh missing -device virtio-blk-device?)\n");
        return;
    }

    TEST_ASSERT(size <= VIRTIO_BLK_PROBE_BLOB_MAX,
                "virtio-blk-probe binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_virtio_blk_probe_blob[i] = src[i];

    uart_puts("    /virtio-blk-probe size=");
    uart_putdec((u64)size);
    uart_puts(" bytes; blk_dev pa=");
    uart_puthex64((u64)blk_dev->pa);
    uart_puts(" → rfork_with_caps(CAP_HW_CREATE)\n");

    struct virtio_blk_probe_exec_args args = {
        .blob = g_virtio_blk_probe_blob, .size = size
    };

    // Grant the child CAP_HW_CREATE (kproc has CAP_ALL = CAP_HW_CREATE
    // at v1.0; AND-with-parent yields CAP_HW_CREATE on the child).
    int pid = rfork_with_caps(RFPROC, virtio_blk_probe_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /virtio-blk-probe");

    // P4-Ic6-impl (R12-sched): wait_pid works directly now. Before
    // R12-sched-impl, this test used a yield-poll pattern (for(;;){
    // sched(); wfi; } until PROC_STATE_ZOMBIE) to keep the parent
    // RUNNABLE while the child slept on the IRQ Rendez — because the
    // scheduler's "no runnable peer system-wide" deadlock check would
    // ELE when both parent (sleeping on child_done) and child (sleeping
    // on hardware IRQ) were SLEEPING simultaneously. P4-Ic6-impl
    // allocates a dedicated boot-CPU idle thread (g_bootcpu_idle) in
    // BAND_IDLE's run tree, so pick_next always finds something to
    // switch to even when both parent and child are SLEEPING. The
    // scheduler switches to bootcpu_idle's WFI loop; the device IRQ
    // arrives, wakes the child; child runs, exits; exit's wakeup wakes
    // the parent. This is the wait_pid path that every other userspace
    // test uses. Closes the R12-sched workaround.
    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0, "/virtio-blk-probe exit status (0 = block 0 read + signature verified)");

    uart_puts("    /virtio-blk-probe reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — full driver SVC path verified end-to-end\n");
}
