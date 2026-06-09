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

// A-4c-1: PL011 RX IRQ handler. Runs in IRQ context (IRQs masked at PSTATE).
// Drains the RX FIFO until empty, splitting each 12-bit DR read into the data
// byte (bits 7:0) and the break flag (bit 10 BE), and hands each entry to the
// console layer. cons_rx_input is wakeup-only (it does no IRQ-unsafe work);
// the privileged/blocking work is deferred to the console_mgr kthread. Clears
// the RX + RX-timeout interrupts before returning so the line de-asserts.
void uart_rx_handler(uint32_t intid, void *arg) {
    (void)intid;
    (void)arg;
    uintptr_t base = pl011_base;
    while (!(mmio_read32(base, PL011_FR) & PL011_FR_RXFE)) {
        uint32_t dr = mmio_read32(base, PL011_DR);
        cons_rx_input((uint8_t)(dr & 0xffu), (dr & PL011_DR_BE) != 0u);
    }
    mmio_write32(base, PL011_ICR, PL011_ICR_RXIC | PL011_ICR_RTIC);
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
