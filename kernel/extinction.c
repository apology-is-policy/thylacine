// Kernel extinction. Per TOOLING.md §10 ABI: the literal string
// "EXTINCTION: " on a fresh line is the agentic-loop's catastrophic-
// failure signal. Don't change without coordinating tools/run-vm.sh,
// tools/test.sh, tools/agent-protocol.md, CLAUDE.md, and TOOLING.md.

#include <thylacine/cons.h>
#include <thylacine/extinction.h>
#include <thylacine/smp.h>

#include "../arch/arm64/halls.h"
#include "../arch/arm64/uart.h"

extern void _torpor(void) __attribute__((noreturn));

// #214: extinction re-entrancy guard. If the dump path itself faults
// (halls_dump dereferencing state the crash already destroyed), the fault
// handler calls extinction again and the pair descend forever, scribbling
// exception frames across RAM (see exception.c's EL1h-sync recursion
// guard — the two catch the same loop at different points). On re-entry:
// print the message with the dump suppressed and park. If even that
// banner faults, the third entry parks silently. Per-CPU so one CPU's
// extinction doesn't gag an independent extinction on a peer.
static volatile u8 g_extinction_depth[DTB_MAX_CPUS];

// Returns only at depth 1 (the normal path). Depth >= 2 parks.
static void extinction_reentry_guard(const char *msg) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    u8 depth = (u8)(g_extinction_depth[cpu] + 1u);
    g_extinction_depth[cpu] = depth;
    if (depth < 2u) return;
    if (depth == 2u) {
        uart_puts("\nEXTINCTION: (recursive on cpu ");
        uart_putdec((u64)cpu);
        uart_puts("; halls dump suppressed) ");
        uart_puts(msg);
        uart_puts("\n");
    }
    _torpor();
}

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
    extinction_reentry_guard(msg);
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
    extinction_reentry_guard(msg);
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
