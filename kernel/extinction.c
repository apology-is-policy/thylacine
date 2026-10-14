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
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>

#include "../arch/arm64/halls.h"
#include "../arch/arm64/timer.h"
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
// try-once would fail in exactly the case it exists for). The THIRD source --
// a peer's direct uart_putc, which is most kernel output, the test runner's
// included -- is closed by the per-byte gate below. Peers keep running after
// the claim, their bytes dropped; stopping them outright is IPI_HALT, still a
// commented-out reservation in smp.h.
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

// The THIRD tearing source: a peer's DIRECT write (uart_putc -- the kernel test
// runner, the boot diagnostics, every uart_puts), which never touches the TX ring
// and so is outside cons_tx_claim_for_dump's reach. Each byte uart_putc emits is
// bracketed by extinction_tx_enter/exit. Once a CPU owns the console, a peer's
// byte is dropped; a byte a peer had already committed to is waited out by the
// winner before its banner. Dekker order: the peer publishes its flag and then
// reads the claim, the winner publishes the claim and then reads every flag,
// each with a full barrier between -- so at least one side sees the other.
// The flag is a depth, not a boolean: an exception taken inside uart_putc may
// print through it again, and its exit must not clear the outer byte's mark.
static volatile u8 g_extinction_tx_busy[DTB_MAX_CPUS];

// A peer's committed byte can sit in uart_putc's bounded TXFF wait (20 ms); past
// this the winner prints anyway, as cons_tx_claim_for_dump does. One bound for
// the whole wait, however many peers are mid-byte.
#define EXTINCTION_TX_WAIT_MAX_NS    (50ull * 1000ull * 1000ull)
#define EXTINCTION_TX_WAIT_MAX_ITERS (1u << 24)

int extinction_tx_gate_core(volatile u8 *busy, const uint32_t *word, u8 owner) {
    (void)__atomic_fetch_add(busy, 1u, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) == 0u || owner) return 1;
    extinction_tx_exit_core(busy);
    return 0;
}

void extinction_tx_exit_core(volatile u8 *busy) {
    (void)__atomic_fetch_sub(busy, 1u, __ATOMIC_RELEASE);
}

int extinction_tx_wait_core(const volatile u8 *busy, unsigned n, unsigned self) {
    u32 iters = 0;
    u64 t0 = 0;
    for (unsigned c = 0; c < n; c++) {
        if (c == self) continue;
        while (__atomic_load_n(&busy[c], __ATOMIC_SEQ_CST) != 0u) {
            if (++iters >= EXTINCTION_TX_WAIT_MAX_ITERS) return 0;
            u64 now = timer_now_ns();                      // 0 before timer_init
            if (t0 == 0) t0 = now;
            else if (now - t0 >= EXTINCTION_TX_WAIT_MAX_NS) return 0;
            __asm__ __volatile__("yield" ::: "memory");
        }
    }
    return 1;
}

// The mark is per CPU, so the byte's thread must not leave the CPU between
// enter and exit: a switch would let another thread's exit clear the mark,
// and a migration would drop it on a CPU that never raised it. With IRQs on,
// the thread's preempt count is held across the byte (IRQs themselves stay on
// through the TXFF wait, #67) and a preempt deferred meanwhile is taken at
// exit. With IRQs masked nothing can switch the thread, and no thread state is
// read: that is every exception path and the whole of extinction(), where
// TPIDR_EL1 may be wild (#806) and a fault on the banner's first byte would
// park the CPU with nothing printed.
int extinction_tx_enter(void) {
    u64 daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    int held = !(daif & (1ull << 7));
    if (held) spin_preempt_inc();
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    if (extinction_tx_gate_core(&g_extinction_tx_busy[cpu], &g_extinction_console,
                                g_extinction_owner[cpu]))
        return held ? EXTINCTION_TX_HELD : EXTINCTION_TX_BYTE;
    if (held) spin_preempt_dec();
    return 0;
}

void extinction_tx_exit(int token) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    extinction_tx_exit_core(&g_extinction_tx_busy[cpu]);
    if (token == EXTINCTION_TX_HELD) {
        spin_preempt_dec();
        sched_preempt_point();
    }
}

// Set if the claim ran with IRQs on: the claiming thread could then be switched
// out mid-wait and resume on a CPU the gate does not admit, every byte of its
// banner dropped. Both extinction entries mask IRQs first, so this reports only
// a path that forgot to (test-fault.sh forbids the line).
static u8 g_extinction_claim_irqs_on;

// TRUE for exactly one CPU per boot. The winner returns only once no peer has a
// byte in flight (or the wait's bound has passed), so the banner it prints next
// starts a line no peer can split.
static int extinction_claim_console(void) {
    if (!extinction_claim_word(&g_extinction_console))
        return 0;
    u64 daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    if (!(daif & (1ull << 7))) g_extinction_claim_irqs_on = 1u;
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) cpu = 0;
    g_extinction_owner[cpu] = 1u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    (void)extinction_tx_wait_core(g_extinction_tx_busy, DTB_MAX_CPUS, cpu);
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
static void extinction_report_claim(void) {
    if (g_extinction_claim_irqs_on)
        uart_puts("  console-claim: taken with irqs on (the claiming thread could leave the owning cpu)\n");
}

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
    // First: every per-CPU fact below (depth, owner, the gate's admission) is
    // this CPU's only while the dying thread cannot be switched out. Never
    // restored: every path from here parks.
    irq_mask_local();
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
    extinction_report_claim();
    _torpor();
}

void extinction_with_addr(const char *msg, uintptr_t addr) {
    irq_mask_local();                       // first, as in extinction()
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
    extinction_report_claim();
    _torpor();
}
