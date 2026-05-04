// Thylacine kernel main entry — boot_main().
//
// Called from arch/arm64/start.S after BSS clear and stack setup. Prints
// the boot banner per TOOLING.md §10 (load-bearing kernel ABI for the
// agentic tooling), then halts.
//
// At P1-A: most banner fields are placeholders. P1-B parses DTB → mem +
// dtb fields. P1-C enables KASLR → kernel base + KASLR offset fields.
// P1-F brings up SMP → cpus field. P1-H enables the full hardening stack
// → hardening field. The placeholder text inside parentheses tracks which
// future sub-chunk fills each field.
//
// The `Thylacine boot OK` line is the agent's boot-success signal per
// TOOLING.md §10. Do not change without a coordinated update to
// `tools/run-vm.sh`, `tools/agent-protocol.md`, `CLAUDE.md`, and
// `TOOLING.md`.

#include "uart.h"

#include <stdint.h>
#include <thylacine/types.h>

// From arch/arm64/start.S — DTB physical address handed to us by the
// bootloader. Populated before boot_main() is called.
extern volatile u64 _saved_dtb_ptr;

// From arch/arm64/kernel.ld.
extern char _kernel_start[];

void boot_main(void);

void boot_main(void) {
    uart_puts("Thylacine v" THYLACINE_VERSION_STRING "-dev booting...\n");

    uart_puts("  arch: arm64\n");

    uart_puts("  cpus: 1 (P1-A; SMP at P1-F)\n");

    uart_puts("  mem:  unknown (DTB at P1-B)\n");

    uart_puts("  dtb:  ");
    uart_puthex64(_saved_dtb_ptr);
    uart_puts("\n");

    uart_puts("  hardening: minimal (P1-A baseline; full stack at P1-H)\n");

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
