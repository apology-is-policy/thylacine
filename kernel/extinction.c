// Kernel extinction. Per TOOLING.md §10 ABI: the literal string
// "EXTINCTION: " on a fresh line is the agentic-loop's catastrophic-
// failure signal. Don't change without coordinating tools/run-vm.sh,
// tools/test.sh, tools/agent-protocol.md, CLAUDE.md, and TOOLING.md.

#include <thylacine/extinction.h>

#include "../arch/arm64/uart.h"

extern void _torpor(void) __attribute__((noreturn));

void extinction(const char *msg) {
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts("\n");
    _torpor();
}

void extinction_with_addr(const char *msg, uintptr_t addr) {
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts(" 0x");
    uart_puthex64((uint64_t)addr);
    uart_puts("\n");
    _torpor();
}
