// Thylacine kernel main entry — boot_main().
//
// Called from arch/arm64/start.S after BSS clear and stack setup. Parses
// the DTB QEMU loaded for us, updates the UART base from DTB, prints the
// boot banner per TOOLING.md §10 (load-bearing kernel ABI for the agentic
// tooling), then halts.
//
// At P1-B: DTB is parsed; mem field shows discovered RAM size; UART base
// confirmed via DTB. Remaining fields (kaslr offset, full hardening, SMP
// CPU count) are filled in at P1-C / P1-F / P1-H respectively.
//
// The `Thylacine boot OK` line is the agent's boot-success signal per
// TOOLING.md §10. Do not change without a coordinated update to
// `tools/run-vm.sh`, `tools/agent-protocol.md`, `CLAUDE.md`, and
// `TOOLING.md`.

#include "uart.h"

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/types.h>

// From arch/arm64/start.S — DTB physical address handed to us by the
// bootloader (in x0 per the Linux ARM64 boot protocol). Populated
// before boot_main() is called.
extern volatile u64 _saved_dtb_ptr;

// From arch/arm64/start.S — 1 if the kernel was entered at EL2 and
// dropped to EL1 by _real_start, 0 if entered at EL1 directly. Used
// for the el-entry banner diagnostic; surfaces a Pi 5 (or other
// EL2-entry firmware) condition that QEMU virt never exhibits.
extern volatile u64 _entered_at_el2;

// From arch/arm64/kernel.ld.
extern char _kernel_start[];
extern char _kernel_end[];

void boot_main(void);

void boot_main(void) {
    // Phase 1: parse the DTB (early prints use the fallback PL011 base
    // 0x09000000 from uart.c; if the DTB places PL011 elsewhere,
    // uart_set_base() below will update it before the banner prints).
    bool dtb_ok = dtb_init((paddr_t)_saved_dtb_ptr);

    u64 mem_base = 0, mem_size = 0;
    bool mem_ok = false;
    u64 dtb_uart_base = 0, dtb_uart_size = 0;
    bool uart_ok = false;

    if (dtb_ok) {
        mem_ok  = dtb_get_memory(&mem_base, &mem_size);
        uart_ok = dtb_get_compat_reg("arm,pl011", &dtb_uart_base, &dtb_uart_size);
        if (uart_ok) {
            uart_set_base((uintptr_t)dtb_uart_base);
        }
    }

    // Banner. Format is kernel ABI per TOOLING.md §10; do not change.
    uart_puts("Thylacine v" THYLACINE_VERSION_STRING "-dev booting...\n");

    uart_puts("  arch: arm64\n");

    uart_puts("  el-entry: ");
    if (_entered_at_el2) {
        uart_puts("EL2 -> EL1 (dropped)\n");
    } else {
        uart_puts("EL1 (direct)\n");
    }

    uart_puts("  cpus: 1 (P1-C; SMP at P1-F)\n");

    uart_puts("  mem:  ");
    if (mem_ok) {
        uart_putdec(mem_size / (1024UL * 1024UL));
        uart_puts(" MiB at ");
        uart_puthex64(mem_base);
    } else {
        uart_puts("unknown (DTB parse failed or /memory absent)");
    }
    uart_puts("\n");

    uart_puts("  dtb:  ");
    uart_puthex64(_saved_dtb_ptr);
    if (!dtb_ok) {
        uart_puts(" (parse FAILED — fallback UART, no memory info)");
    } else {
        uart_puts(" (parsed)");
    }
    uart_puts("\n");

    uart_puts("  uart: ");
    uart_puthex64((u64)uart_get_base());
    if (uart_ok) {
        uart_puts(" (DTB-driven)");
    } else if (dtb_ok) {
        uart_puts(" (fallback; arm,pl011 absent in DTB)");
    } else {
        uart_puts(" (fallback; DTB unavailable)");
    }
    uart_puts("\n");

    uart_puts("  hardening: MMU+W^X+extinction (P1-C; KASLR/PAC/MTE/CFI at later sub-chunks)\n");

    uart_puts("  kernel base: ");
    uart_puthex64((u64)(uintptr_t)_kernel_start);
    uart_puts(" (KASLR at P1-C)\n");

    uart_puts("  phase: " THYLACINE_PHASE_STRING "\n");

    uart_puts("Thylacine boot OK\n");

    // boot_main() must not return. start.S has a fallthrough to _hang
    // for safety, but be explicit here too.
    extern void _hang(void) __attribute__((noreturn));
    _hang();
}
