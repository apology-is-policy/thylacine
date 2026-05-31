// Kernel extinction. Per TOOLING.md §10 ABI: the literal string
// "EXTINCTION: " on a fresh line is the agentic-loop's catastrophic-
// failure signal. Don't change without coordinating tools/run-vm.sh,
// tools/test.sh, tools/agent-protocol.md, CLAUDE.md, and TOOLING.md.

#include <thylacine/extinction.h>

#include "../arch/arm64/halls.h"
#include "../arch/arm64/uart.h"

extern void _torpor(void) __attribute__((noreturn));

void extinction(const char *msg) {
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts("\n");
    // HX-1: the "EXTINCTION: " ABI line above is emitted first + unchanged
    // (TOOLING.md section 10); the Halls crash dump follows under "HALLS:".
    // NULL -> halls_dump consults the per-CPU live exception frame, falling
    // back to capture-current for a bare assert.
    halls_dump((void *)0);
    _torpor();
}

void extinction_with_addr(const char *msg, uintptr_t addr) {
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts(" 0x");
    uart_puthex64((uint64_t)addr);
    uart_puts("\n");
    halls_dump((void *)0);
    _torpor();
}
