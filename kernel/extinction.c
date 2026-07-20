// Kernel extinction. Per TOOLING.md §10 ABI: the literal string
// "EXTINCTION: " on a fresh line is the agentic-loop's catastrophic-
// failure signal. Don't change without coordinating tools/run-vm.sh,
// tools/test.sh, tools/agent-protocol.md, CLAUDE.md, and TOOLING.md.

#include <thylacine/cons.h>
#include <thylacine/extinction.h>

#include "../arch/arm64/halls.h"
#include "../arch/arm64/uart.h"

extern void _torpor(void) __attribute__((noreturn));

// #75 / P1-F: console output now stages through a ring the TX interrupt drains,
// and a dying machine runs IRQ-masked -- so anything still in the ring would be
// lost. Flush it (bounded, trylock-only) BEFORE the "EXTINCTION: " line so the
// output that led up to the crash is on the wire and in causal order. Bounded +
// non-recursing per HX-I; if a peer CPU holds the ring lock we skip rather than
// wedge the dump.
static void extinction_flush_console(void) {
    cons_tx_flush_for_dump();
}

void extinction(const char *msg) {
    extinction_flush_console();
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
    extinction_flush_console();
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts(" ");          // uart_puthex64 emits its own "0x" prefix
    uart_puthex64((uint64_t)addr);
    uart_puts("\n");
    halls_dump((void *)0);
    _torpor();
}
