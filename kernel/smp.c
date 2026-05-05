// SMP secondary bring-up (P2-Ca).
//
// Iterates DTB cpus, brings each non-boot secondary up via PSCI_CPU_ON
// pointing at the asm trampoline secondary_entry (in start.S). The
// trampoline runs at low PA (no MMU), flips g_cpu_online[idx], and
// parks at WFI. Primary waits for the flag with a per-CPU timeout.
//
// At P2-Ca secondaries are alive but cannot execute kernel code beyond
// the trampoline (no MMU, no PAC, no per-CPU exception stack). P2-Cb
// adds full per-CPU init bringing them into the scheduler.

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/smp.h>
#include <thylacine/types.h>

#include "../arch/arm64/kaslr.h"
#include "../arch/arm64/psci.h"
#include "../arch/arm64/timer.h"
#include "../arch/arm64/uart.h"

// Linker-defined kernel image bounds — used to compute secondary_entry's PA.
extern char _kernel_start[];

// Asm trampoline label (defined in arch/arm64/start.S).
extern void secondary_entry(void);

// Per-CPU online flag. Set by secondary_entry's trampoline (asm strb)
// before the WFI loop. Read by smp_init's wait loop with `volatile` to
// force the compiler to re-read each iteration.
volatile u8 g_cpu_online[DTB_MAX_CPUS];

static unsigned g_cpu_count;
static unsigned g_cpu_online_count;

// Per-secondary timeout. PSCI bring-up + trampoline execution + flag
// store + dsb sy is microseconds at most on QEMU virt; bare metal might
// be milliseconds. 100 ms is generous.
#define SMP_BRINGUP_TIMEOUT_TICKS  100

// Compute the PA of secondary_entry. The trampoline is part of the
// kernel image (.text); its VA was assigned by the linker to
// KASLR_LINK_VA + offset, then patched by .rela.dyn relocations. The
// PA is kaslr_kernel_pa_start() + (VA - link_VA_of_kernel_start).
//
// Equivalently: PA = kaslr_kernel_pa_start() + (VA - link_kernel_start_VA).
// Since (link_kernel_start_VA) is the base of the kernel image's load
// address in the original linker script, and at runtime
// kaslr_kernel_pa_start() returns the actual load PA, the offset
// (secondary_entry - _kernel_start) is the same VA-relative offset
// regardless of the slide.
static u64 compute_secondary_entry_pa(void) {
    u64 entry_va  = (u64)(uintptr_t)secondary_entry;
    u64 kernel_va = (u64)(uintptr_t)_kernel_start;
    u64 offset    = entry_va - kernel_va;
    return kaslr_kernel_pa_start() + offset;
}

// Translate a PSCI return code to a uart-printable summary.
static const char *psci_status_str(int status) {
    switch (status) {
    case 0:    return "SUCCESS";
    case -1:   return "NOT_SUPPORTED";
    case -2:   return "INVALID_PARAMETERS";
    case -3:   return "DENIED";
    case -4:   return "ALREADY_ON";
    case -5:   return "ON_PENDING";
    case -6:   return "INTERNAL_FAILURE";
    case -7:   return "NOT_PRESENT";
    case -8:   return "DISABLED";
    case -9:   return "INVALID_ADDRESS";
    default:   return "UNKNOWN";
    }
}

// Wait for g_cpu_online[idx] to become true with timeout. Returns true
// if it came online; false on timeout. The polling loop reads through
// `volatile` so the compiler re-fetches each iteration; the explicit
// `dmb ish` ensures the read happens after any pending inner-shareable
// store (the secondary's strb + dsb sy).
static bool wait_for_online(unsigned idx, u64 timeout_ticks) {
    u64 start = timer_get_ticks();
    while (!g_cpu_online[idx]) {
        if ((timer_get_ticks() - start) > timeout_ticks) return false;
        __asm__ __volatile__("dmb ish\n" ::: "memory");
        // No WFI here — IRQs may not be live; we want to spin so we
        // notice the flag promptly.
    }
    return true;
}

unsigned smp_init(void) {
    g_cpu_count = dtb_cpu_count();
    if (g_cpu_count == 0) {
        // Malformed DTB — shouldn't happen. Treat as UP.
        g_cpu_count = 1;
        g_cpu_online[0] = 1;
        g_cpu_online_count = 1;
        return 0;
    }

    // Mark boot CPU online unconditionally.
    g_cpu_online[0] = 1;
    g_cpu_online_count = 1;

    // PSCI required for secondary bring-up.
    if (!psci_is_ready()) {
        uart_puts("  smp: PSCI not available — secondaries held\n");
        return 0;
    }

    if (g_cpu_count > DTB_MAX_CPUS) {
        uart_puts("  smp: cpu count exceeds DTB_MAX_CPUS — capping\n");
        g_cpu_count = DTB_MAX_CPUS;
    }

    u64 entry_pa = compute_secondary_entry_pa();

    unsigned brought_up = 0;
    for (unsigned i = 1; i < g_cpu_count; i++) {
        u64 mpidr = 0;
        if (!dtb_cpu_mpidr(i, &mpidr)) continue;

        int rc = psci_cpu_on(mpidr, entry_pa, /*context_id=*/(u64)i);

        if (rc == 0 || rc == -4 /*ALREADY_ON*/) {
            // Wait for the trampoline to set the online flag.
            if (wait_for_online(i, SMP_BRINGUP_TIMEOUT_TICKS)) {
                brought_up++;
                g_cpu_online_count++;
            } else {
                uart_puts("  smp: cpu ");
                uart_putdec((u64)i);
                uart_puts(" PSCI ok but online-flag timed out\n");
            }
        } else {
            uart_puts("  smp: cpu ");
            uart_putdec((u64)i);
            uart_puts(" PSCI_CPU_ON failed (");
            uart_puts(psci_status_str(rc));
            uart_puts(")\n");
        }
    }

    return brought_up;
}

unsigned smp_cpu_count(void) {
    return g_cpu_count;
}

unsigned smp_cpu_online_count(void) {
    return g_cpu_online_count;
}
