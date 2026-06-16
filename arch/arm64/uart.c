// PL011 UART driver — minimal polled I/O for early boot.
//
// QEMU virt maps the primary PL011 at 0x09000000 by default, but the
// authoritative source is the DTB ("arm,pl011" compatible). At P1-B the
// base starts at the QEMU virt fallback and gets updated to the DTB-
// discovered address by boot_main(). Invariant I-15 (DTB-driven
// discovery) is satisfied for normal operation; the fallback is a
// recovery path used only between kernel entry and dtb_init().
//
// PL011 register layout (PrimeCell UART Technical Reference Manual, ARM
// DDI 0183, sections 3.3.x):
//   +0x000  DR     Data Register (write to send, read to receive).
//   +0x004  RSR    Receive Status / Error Clear.
//   +0x018  FR     Flag Register. Bit 5 = TXFF (TX FIFO full / TX busy).
//   +0x024  IBRD   Integer Baud Rate Divisor.
//   +0x028  FBRD   Fractional Baud Rate Divisor.
//   +0x02C  LCR_H  Line Control Register.
//   +0x030  CR     Control Register.
//
// At P1-B: rely on QEMU's default UART state. P1-F (full UART driver
// with IRQ) extends this with proper init.

#include "uart.h"

#include "mmu.h"

#include <stdint.h>
#include <thylacine/cons.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/spinlock.h>

#define PL011_DR    0x000
#define PL011_FR    0x018
#define PL011_FR_TXFF  (1u << 5)   // TX FIFO full / busy

// A-4c-1: RX-side register layout (ARM DDI 0183 PrimeCell PL011 TRM).
#define PL011_FR_RXFE     (1u << 4)    // RX FIFO empty
#define PL011_DR_BE       (1u << 10)   // break error flag in a DR read
#define PL011_IMSC        0x038        // interrupt mask set/clear
#define PL011_ICR         0x044        // interrupt clear (write-1-to-clear)
#define PL011_IMSC_RXIM   (1u << 4)    // RX interrupt mask
#define PL011_IMSC_RTIM   (1u << 6)    // RX-timeout interrupt mask
#define PL011_ICR_RXIC    (1u << 4)    // clear RX interrupt
#define PL011_ICR_RTIC    (1u << 6)    // clear RX-timeout interrupt
#define PL011_CR          0x030        // control register (UARTEN/TXE/RXE)
#define PL011_CR_UARTEN   (1u << 0)    // UART master enable
#define PL011_CR_RXE      (1u << 9)    // receive enable

// Active PL011 base. Defaults to QEMU virt fallback so prints work
// during early boot (before DTB parsing). uart_set_base() updates this
// once the DTB is parsed.
//
// `volatile` is overkill here (the writer is single-threaded and runs
// before any interrupts are enabled), but it documents that this is
// shared state and prevents the compiler from caching it across calls.
static volatile uintptr_t pl011_base = 0x09000000UL;

void uart_set_base(uintptr_t base) {
    if (base != 0) {
        pl011_base = base;
    }
}

void uart_remap_to_vmalloc(uintptr_t pa, size_t size) {
    if (pa == 0 || size == 0) return;
    void *kva = mmu_map_mmio((paddr_t)pa, size);
    if (!kva) {
        // mmu_map_mmio extincts on internal errors; only NULL path is
        // size==0 which we filtered above. Defense-in-depth.
        extinction("uart_remap_to_vmalloc: mmu_map_mmio returned NULL");
    }
    pl011_base = (uintptr_t)kva;
}

uintptr_t uart_get_base(void) {
    return pl011_base;
}

// Volatile MMIO accessors. ARM64 needs DSB after writes that must be ordered
// with subsequent operations; for a polled-on-flag UART the natural order is
// preserved by the FR-poll loop, so we don't need explicit barriers here.
static inline void mmio_write32(uintptr_t base, uintptr_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint32_t mmio_read32(uintptr_t base, uintptr_t off) {
    return *(volatile uint32_t *)(base + off);
}

// #174 RX backpressure state. g_uart_rx_lock serializes the RX FIFO drain, the
// IMSC.RXIM/RTIM mask/unmask, and g_rx_paused -- so the IRQ handler (which
// pauses on a full cons ring) and the cons reader's uart_rx_pump (which resumes)
// never race on the FIFO or the mask register. Lock order: g_uart_rx_lock ->
// g_cons.lock (cons_rx_input, called under the RX lock, takes g_cons.lock);
// the cons reader RELEASES g_cons.lock before calling uart_rx_pump, so there is
// no g_cons.lock -> g_uart_rx_lock edge -- the order is acyclic. g_rx_paused is
// additionally an ACQUIRE/RELEASE atomic so uart_rx_pump's fast path (not
// paused) needs no lock.
static spin_lock_t g_uart_rx_lock = SPIN_LOCK_INIT;
static bool        g_rx_paused;            // true: RX masked, bytes held in FIFO

// Mask or unmask the PL011 RX + RX-timeout interrupts. Caller holds
// g_uart_rx_lock. Masking (not clearing ICR) is what makes resumption
// wedge-safe: it gates the GIC line without touching int_level, so nothing is
// stranded by the QEMU "RXRIS is receive-set, not level-recomputed" quirk that
// caused #172 -- resumption is driven by uart_rx_pump reading the FIFO, never by
// hoping the interrupt re-fires.
static void uart_rx_set_enabled(bool en) {
    uintptr_t base = pl011_base;
    uint32_t imsc = mmio_read32(base, PL011_IMSC);
    if (en) imsc |= (PL011_IMSC_RXIM | PL011_IMSC_RTIM);
    else    imsc &= ~(uint32_t)(PL011_IMSC_RXIM | PL011_IMSC_RTIM);
    mmio_write32(base, PL011_IMSC, imsc);
}

void uart_putc(char c) {
    uintptr_t base = pl011_base;
    // Spin until TX FIFO has room (TXFF clear).
    while (mmio_read32(base, PL011_FR) & PL011_FR_TXFF) {
        // Busy-wait. This is fine at P1-B (single CPU, no scheduler).
        // P1-F adds IRQ-driven TX with a buffer.
    }
    mmio_write32(base, PL011_DR, (uint32_t)(unsigned char)c);
}

// A-4c-1: enable the PL011 RX path. QEMU's virt PL011 comes up in its reset
// state (CR=0x300: TXE|RXE set, UARTEN clear) and is lenient about TX without
// UARTEN -- which is why kernel prints work even though the UART is master-
// disabled -- but it does NOT receive bytes or raise the RX IRQ until UARTEN
// is set. So set UARTEN|RXE first (a pure OR: preserves TXE + cannot disturb
// the live TX path), then clear stale RX raw-interrupt state + unmask
// IMSC.RXIM|RTIM so a received byte (or an RX-FIFO-timeout with data pending)
// raises the UART SPI.
void uart_rx_init(void) {
    uintptr_t base = pl011_base;
    uint32_t cr = mmio_read32(base, PL011_CR);
    mmio_write32(base, PL011_CR, cr | PL011_CR_UARTEN | PL011_CR_RXE);
    mmio_write32(base, PL011_ICR, PL011_ICR_RXIC | PL011_ICR_RTIC);
    uint32_t imsc = mmio_read32(base, PL011_IMSC);
    mmio_write32(base, PL011_IMSC, imsc | PL011_IMSC_RXIM | PL011_IMSC_RTIM);
}

// Regression guard (#943): the RX path is live iff the UART is master-enabled
// (CR.UARTEN) AND receive-enabled (CR.RXE). QEMU's PL011 resets with UARTEN
// clear; before uart_rx_init set it, the console silently never received.
bool uart_rx_path_enabled(void) {
    uint32_t cr = mmio_read32(pl011_base, PL011_CR);
    return (cr & PL011_CR_UARTEN) != 0u && (cr & PL011_CR_RXE) != 0u;
}

// Max bytes drained per RX IRQ. BOUNDED to break the freeze (#172): an
// unbounded `while (!RXFE)` drain livelocked the CPU under sustained input.
// QEMU's chardev refills the PL011 RX FIFO as fast as the guest drains it --
// especially under HVF, where QEMU's main loop runs on a PARALLEL host thread
// and tops the FIFO up concurrently with the guest's drain, so RXFE never
// became true and the handler never returned (IRQs masked -> no timer tick, no
// preempt -> whole-OS freeze; reproduced holding an arrow key in nora AND in
// the cooked ut shell, both accels -- TCG needs faster input since its vCPU +
// main loop are serialized). 64 = 4 PL011 FIFO depths: a normal burst drains in
// one IRQ, the worst-case IRQ-masked window stays well under a 1ms timer tick.
#define UART_RX_DRAIN_MAX  64u

// The shared PL011 RX FIFO drain core. Caller holds g_uart_rx_lock. Clears the
// RX + RX-timeout interrupts FIRST, then drains the FIFO into the console -- but
// BEFORE reading each byte it checks cons_rx_can_accept(): when the cons ring is
// full it STOPS, leaving the byte in the FIFO (it is NOT read out, so NOT lost),
// masks RX, and latches g_rx_paused (#174 backpressure). The FIFO then fills,
// QEMU's can_receive goes 0, and the host serial buffers the overflow -- nothing
// is dropped. uart_rx_pump (the reader-side resume) drains the held bytes once
// ring space frees.
//
// Clear-FIRST is the #172 fix: the QEMU PL011 RX interrupt is RECEIVE-driven and
// NOT recomputed from the FIFO level on an ICR clear (confirmed via gdb: FIFO
// FULL yet RXRIS clear), so clearing AFTER the drain would strand a byte
// arriving in the post-drain window and wedge the FIFO. Clearing first re-arms
// for any drain-window arrival; the drain reads every byte already latched.
// UART_RX_DRAIN_MAX is the separate unbounded-drain livelock bound; both stay.
//
// Returns true iff it reached FIFO-empty (RXFE). false means either backpressure
// (g_rx_paused now set + RX masked) OR budget-hit with the FIFO non-empty -- the
// pump treats both as "stay paused / retry", the handler ignores it (a budget-hit
// re-fires per #172; a backpressure-pause is resumed by the reader's pump).
static bool uart_rx_drain_locked(void) {
    uintptr_t base = pl011_base;
    mmio_write32(base, PL011_ICR, PL011_ICR_RXIC | PL011_ICR_RTIC);   // #172 clear-first

    unsigned budget = UART_RX_DRAIN_MAX;
    while (budget != 0u) {
        if (mmio_read32(base, PL011_FR) & PL011_FR_RXFE) return true;   // FIFO drained
        if (!cons_rx_can_accept()) {
            // #174: cons ring full -> backpressure. Leave the byte in the FIFO
            // (do NOT read DR), mask RX so the IRQ neither re-fires nor
            // livelocks, latch paused. uart_rx_pump resumes from the reader side.
            uart_rx_set_enabled(false);
            __atomic_store_n(&g_rx_paused, true, __ATOMIC_RELEASE);
            return false;
        }
        budget--;
        uint32_t dr = mmio_read32(base, PL011_DR);
        cons_rx_input((uint8_t)(dr & 0xffu), (dr & PL011_DR_BE) != 0u);
    }
    return false;   // budget hit, FIFO non-empty: re-fire (#172) / pump-retry
}

// A-4c-1: PL011 RX IRQ handler (GIC dispatch target; IRQ context, IRQs masked at
// PSTATE). Drains the FIFO under g_uart_rx_lock via the shared core; on a full
// cons ring it pauses RX (#174) rather than dropping bytes. After the handler
// returns + the GIC EOIs, the exception-return window lets the lower-INTID timer
// (27 < the UART SPI 33, equal GIC priority -> the timer wins the tie) interleave
// so the scheduler runs.
void uart_rx_handler(uint32_t intid, void *arg) {
    (void)intid;
    (void)arg;
    irq_state_t s = spin_lock_irqsave(&g_uart_rx_lock);
    (void)uart_rx_drain_locked();
    spin_unlock_irqrestore(&g_uart_rx_lock, s);
}

// #174 backpressure resume (process context; the cons reader calls this after it
// frees ring space). No-op unless RX is paused. While paused, RX is masked so the
// IRQ handler cannot run -- this pump is then the SOLE FIFO drainer (and the cons
// single-reader guard means at most one pump runs at a time), so there is no FIFO
// race. It drains held bytes into the freed ring space; when the FIFO empties it
// unmasks RX and clears paused (resumption is reader-driven, never int_level-
// driven -> no #172 wedge). If the ring refills mid-pump it re-pauses; if the
// budget is hit with the FIFO still non-empty (a sustained HVF refill) it stays
// paused for the next reader read -- never stranding a byte.
void uart_rx_pump(void) {
    if (!__atomic_load_n(&g_rx_paused, __ATOMIC_ACQUIRE)) return;   // fast path: not paused
    irq_state_t s = spin_lock_irqsave(&g_uart_rx_lock);
    if (g_rx_paused) {
        if (uart_rx_drain_locked()) {
            // FIFO drained with ring space remaining -> resume normal IRQ-driven
            // RX. Unmask BEFORE clearing paused so a fresh arrival re-fires the
            // (now-unmasked) IRQ instead of waiting for the next pump.
            uart_rx_set_enabled(true);
            __atomic_store_n(&g_rx_paused, false, __ATOMIC_RELEASE);
        }
        // else: ring refilled (drain re-latched paused) or budget hit -> stay
        // paused; the next reader read pumps again. No byte stranded.
    }
    spin_unlock_irqrestore(&g_uart_rx_lock, s);
}

void uart_puts(const char *s) {
    while (*s) {
        // Translate '\n' to '\r\n' so output looks correct on a real
        // terminal (and on QEMU's `-serial mon:stdio` host-side tty,
        // which doesn't itself do CR translation).
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
        s++;
    }
}

void uart_puthex64(uint64_t v) {
    static const char hexdigits[] = "0123456789abcdef";
    uart_putc('0');
    uart_putc('x');
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hexdigits[(v >> i) & 0xF]);
    }
}

void uart_putdec(uint64_t v) {
    // Max uint64 is 20 decimal digits.
    char buf[21];
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    // Buffer is in reverse order; emit highest-order digit first.
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}
