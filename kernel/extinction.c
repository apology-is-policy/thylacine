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
// The SECOND tearing source -- a peer's NORMAL console write landing inside the
// banner -- is closed one step below by cons_tx_claim_for_dump: the winner takes
// the console TX ring lock (every steady-state push and every ring->FIFO drain
// runs under it), by a BOUNDED raw try-spin, and holds it forever. Different
// primitive, different shape, on purpose: THIS word's holder is a dying peer
// that never releases (try-once is right); the ring's holder is a healthy peer
// mid-push that will release in microseconds (a bounded spin is right, and a
// try-once would fail in exactly the case it exists for). Still owed: IPI_HALT,
// so the winner STOPS its peers rather than out-racing them -- a commented-out
// reservation in smp.h; it would also cover the residual this leaves, the few
// kernel diagnostics that still write the UART directly instead of through the
// ring (they are outside the ring lock's reach).
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

// For the ONE other emitter of the ABI line -- exception.c's el1_sync_runaway,
// reached from a fault chain that may already have claimed the console on THIS
// CPU (extinction -> halls_dump faults -> ... -> runaway). Ownership is not a
// loss: the owner may print. A PEER holding it means a peer is dumping, and the
// caller parks silent like any loser (counted, so the winner reports it).
int extinction_console_claim_or_own(void) {
    if (extinction_claim_console()) return 1;
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    if (g_extinction_owner[cpu]) return 1;
    __atomic_fetch_add(&g_extinction_suppressed, 1u, __ATOMIC_RELAXED);
    return 0;
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

// #75 / P1-F: console output stages through a ring the TX interrupt drains, and
// a dying machine runs IRQ-masked -- so anything still in the ring would be
// lost. cons_tx_claim_for_dump takes the ring lock (bounded raw try-spin, never
// a park), flushes the pre-crash ring to the wire under it, and RETURNS HOLDING
// IT: from here to _torpor no peer can push or drain a byte, so the ABI line
// below cannot be torn by a normal console write. Past its bound (a peer kept
// the lock longer than any healthy holder does) we emit anyway -- torn beats
// silent -- and say so after the dump. Recorded per-CPU by the owner so the
// report cannot be faked by a loser (which never gets here).
static u8 g_extinction_ring_held[DTB_MAX_CPUS];

static void extinction_claim_ring(void) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    g_extinction_ring_held[cpu] = cons_tx_claim_for_dump() ? 1u : 0u;
}

// Printed by the winner AFTER halls_dump (the ABI line stays first). Same
// wording rules as the peer report: no "EXTINCTION" token, nothing at column 0.
static void extinction_report_ring(void) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    if (g_extinction_ring_held[cpu]) {
        uart_puts("  console-ring: held by the dumping cpu (banner serialized against peers)\n");
    } else {
        uart_puts("  console-ring: NOT held (a peer kept it past the bound; the banner above may be torn)\n");
    }
}

void extinction(const char *msg) {
    extinction_reentry_guard(msg);
    if (!extinction_claim_console()) extinction_park_suppressed();
    extinction_claim_ring();
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
    extinction_report_ring();
    _torpor();
}

void extinction_with_addr(const char *msg, uintptr_t addr) {
    extinction_reentry_guard(msg);
    if (!extinction_claim_console()) extinction_park_suppressed();
    extinction_claim_ring();
    uart_puts("\n");
    uart_puts("EXTINCTION: ");
    uart_puts(msg);
    uart_puts(" ");          // uart_puthex64 emits its own "0x" prefix
    uart_puthex64((uint64_t)addr);
    uart_puts("\n");
#ifdef THYLACINE_FAULT_TEST_el1_sync_runaway
    // #246: the ONLY reachable route to el1_sync_runaway -- take EL1-sync depth
    // from 2 (where the #806 guard called us) to EL1_SYNC_DEPTH_MAX. Placed
    // HERE, after the ring claim and the ABI line and BEFORE halls_dump,
    // deliberately: a regression test wants ONE determined fault site, and
    // letting halls_dump fault first would reach the same arm from a place that
    // varies with the dump's contents. It also puts the runaway's claim-or-own
    // and cons_tx_claim_for_dump on their ALREADY-OWNED-BY-THIS-CPU arms, which
    // no other test constructs. Absent from every build that does not define
    // this variant -- verify the production ELF is byte-identical, do not
    // assume it.
    {
        volatile uint64_t *q = (volatile uint64_t *)0xffff999911000000ULL;
        __asm__ __volatile__("" : "+r"(q));
        *q = 0xdeadu;
    }
#endif
    halls_dump((void *)0);
    extinction_report_suppressed();
    extinction_report_ring();
    _torpor();
}
