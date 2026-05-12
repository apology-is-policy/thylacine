// /virtio-net-arp regression test (P4-Jb).
//
// Successor to test_virtio_net_probe.c. Where the probe proves
// TX-virtqueue mechanics in isolation, the arp test closes the
// round-trip by adding RX setup + parsing the slirp ARP reply. This
// is the first userspace process that observes a frame arriving from
// outside-the-guest.
//
// Same shape as test_virtio_net_probe: looks up the binary in the
// ramfs; verifies a virtio-net device is wired; rforks a child with
// CAP_HW_CREATE; waits via wait_pid; asserts exit 0.

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

void test_virtio_net_arp_rfork_with_caps(void);

#define VIRTIO_NET_ARP_BLOB_MAX 131072
static _Alignas(16) u8 g_virtio_net_arp_blob[VIRTIO_NET_ARP_BLOB_MAX];

struct virtio_net_arp_exec_args {
    const void *blob;
    size_t      size;
};

__attribute__((noreturn))
static void virtio_net_arp_exec_thunk(void *arg) {
    struct virtio_net_arp_exec_args *ea =
        (struct virtio_net_arp_exec_args *)arg;
    struct Thread *t = current_thread();
    if (!t) extinction("virtio_net_arp_exec_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("virtio_net_arp_exec_thunk: no proc");

    if ((p->caps & CAP_HW_CREATE) == 0) {
        uart_puts("    virtio_net_arp_exec_thunk: child lacks CAP_HW_CREATE\n");
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

void test_virtio_net_arp_rfork_with_caps(void) {
    const void *cpio_blob = NULL;
    size_t size = 0;

    int rc = devramfs_lookup("virtio-net-arp", &cpio_blob, &size);
    if (rc != 0) {
        uart_puts("    [skip] /virtio-net-arp not in ramfs (build with: tools/build.sh all)\n");
        return;
    }

    struct virtio_mmio_dev *net_dev =
        virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_NET);
    if (!net_dev) {
        uart_puts("    [skip] no virtio-mmio slot with DeviceID=1\n");
        return;
    }

    TEST_ASSERT(size <= VIRTIO_NET_ARP_BLOB_MAX,
                "virtio-net-arp binary too large for static buffer");

    const u8 *src = (const u8 *)cpio_blob;
    for (size_t i = 0; i < size; i++) g_virtio_net_arp_blob[i] = src[i];

    uart_puts("    /virtio-net-arp size=");
    uart_putdec((u64)size);
    uart_puts(" bytes; net_dev pa=");
    uart_puthex64((u64)net_dev->pa);
    uart_puts(" → rfork_with_caps(CAP_HW_CREATE)\n");

    struct virtio_net_arp_exec_args args = {
        .blob = g_virtio_net_arp_blob, .size = size
    };

    int pid = rfork_with_caps(RFPROC, virtio_net_arp_exec_thunk, &args,
                              CAP_HW_CREATE);
    TEST_ASSERT(pid > 0, "rfork_with_caps failed for /virtio-net-arp");

    int status = -42;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid pid mismatch");
    TEST_EXPECT_EQ(status, 0,
        "/virtio-net-arp exit status (0 = ARP request/reply round-trip verified)");

    uart_puts("    /virtio-net-arp reaped pid=");
    uart_putdec((u64)pid);
    uart_puts(" status=");
    uart_putdec((u64)status);
    uart_puts(" — TX + RX virtqueue + slirp ARP reply end-to-end\n");
}
