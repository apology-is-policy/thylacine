// Kernel extinction. Per TOOLING.md §10 ABI: the literal string
// "EXTINCTION: " on a fresh line is the agentic-loop's catastrophic-
// failure signal. The authority for who mirrors that string is the vault's
// abi-boot-banner note and its `mirrors` set, reached via `quaestor owner`
// -- not a list transcribed here. The list that used to sit on this line
// named tools/agent-protocol.md, which was planned in Phase 1 and never
// written, and tools/run-vm.sh, which matches neither literal because it
// only launches QEMU and never reads boot output.

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

// The console claim. The re-entrancy guard above is deliberately PER-CPU, so
// two CPUs extincting concurrently both proceed -- and each emits the ABI line
// as four separate unlocked uart_puts calls, which interleave on the wire. The
// failure that makes this worth a lock is not cosmetic: every gate classifies
// on the literal "EXTINCTION: ", so a torn prefix is not "a messy line", it is
// a real extinction the harness cannot see. That is fail-OPEN on the one
// channel the whole test discipline trusts.
//
// A raw atomic rather than a kernel spinlock ON PURPOSE: this runs on a machine
// that is already dying, frequently from inside a fault handler, so anything
// carrying lock-order assertions or debug bookkeeping could itself fault and
// turn a diagnosable crash into a silent one.
//
// TRY-once, never spin. The winner NEVER releases -- every path through
// extinction() ends in _torpor() -- so a spin loop could only ever burn its
// full bound and then park anyway. Losing CPUs park immediately and silently.
//
// Dropping a loser's banner is the right trade against garbling the winner's,
// because the two failure modes are not symmetric: a torn line can be
// classified as a clean boot (fail-open), while a missing one leaves the guest
// visibly hung or timed out (fail-visible). We take the loud failure.
//
// Still owed (the second half of the filed fix): IPI_HALT, so the winner stops
// its peers rather than racing them to a lock they will lose. That needs the
// IPI path wired -- IPI_HALT is currently a commented-out reservation in
// smp.h -- and is tracked separately; this closes the garbling half.
static uint32_t g_extinction_console;

// Best-effort count of peers whose banner we suppressed, printed by the winner
// AFTER the dump so the ABI line stays first and pristine. Racy-low by
// construction (a peer may increment after the winner reads it), hence "at
// least" in the wording -- a diagnostic, never a verdict.
static uint32_t g_extinction_suppressed;

// Set on the CPU that won, so the re-entrancy banner below can require
// ownership STRUCTURALLY. Without it, "only the owner ever reaches depth 2"
// rests on an argument about which code can fault -- true today (a loser parks
// through an atomic add and a WFI loop, neither of which can trap) but exactly
// the kind of premise that stops holding quietly.
static u8 g_extinction_owner[DTB_MAX_CPUS];

// The pure core (see extinction.h). Split out so a test can exercise the
// exactly-one-winner property WITHOUT touching the live console word.
int extinction_claim_word(uint32_t *word) {
    return __atomic_exchange_n(word, 1u, __ATOMIC_ACQ_REL) == 0u;
}

int extinction_console_claimed(void) {
    return __atomic_load_n(&g_extinction_console, __ATOMIC_RELAXED) != 0u;
}

// TRUE for exactly one CPU per boot.
static int extinction_claim_console(void) {
    if (!extinction_claim_word(&g_extinction_console))
        return 0;
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    g_extinction_owner[cpu] = 1u;
    return 1;
}

// A CPU that lost the claim. Counts itself so the winner can report that peers
// died too, then parks without emitting a byte.
static void extinction_park_suppressed(void) __attribute__((noreturn));
static void extinction_park_suppressed(void) {
    __atomic_fetch_add(&g_extinction_suppressed, 1u, __ATOMIC_RELAXED);
    _torpor();
}

// Printed by the winner AFTER halls_dump, so the ABI line stays first. The
// wording deliberately contains neither "EXTINCTION" nor "EXTINCTION:" and
// starts no line at column 0: some gates match the bare token, not just the
// colon form, so a peer note that looked like a banner would be counted as a
// SECOND extinction by exactly the tooling this whole change exists to keep
// honest.
static void extinction_report_suppressed(void) {
    uint32_t n = __atomic_load_n(&g_extinction_suppressed, __ATOMIC_RELAXED);
    if (n == 0u) return;
    uart_puts("  peer-cpus-also-died: at least ");
    uart_putdec((u64)n);
    uart_puts(" (banners suppressed to keep the line above intact)\n");
}

// Returns only at depth 1 (the normal path). Depth >= 2 parks.
static void extinction_reentry_guard(const char *msg) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    u8 depth = (u8)(g_extinction_depth[cpu] + 1u);
    g_extinction_depth[cpu] = depth;
    if (depth < 2u) return;
    if (depth == 2u && g_extinction_owner[cpu]) {
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
    if (!extinction_claim_console()) extinction_park_suppressed();
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
    extinction_report_suppressed();
    _torpor();
}

void extinction_with_addr(const char *msg, uintptr_t addr) {
    extinction_reentry_guard(msg);
    if (!extinction_claim_console()) extinction_park_suppressed();
    extinction_flush_console();
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts(" ");          // uart_puthex64 emits its own "0x" prefix
    uart_puthex64((uint64_t)addr);
    uart_puts("\n");
    halls_dump((void *)0);
    extinction_report_suppressed();
    _torpor();
}
