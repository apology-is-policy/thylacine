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
#include <thylacine/extinction.h>
#include <thylacine/page.h>

#define PL011_DR    0x000
#define PL011_FR    0x018
#define PL011_FR_TXFF  (1u << 5)   // TX FIFO full / busy

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
