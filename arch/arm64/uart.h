// PL011 UART driver — minimal polled I/O for early boot.
//
// Used by boot_main() and any panic path that needs to print before the
// real device infrastructure is up. Replaced by /dev/cons (kernel Dev,
// per ARCH §9.4) at Phase 3.
//
// At P1-A: the PL011 base was hardcoded to 0x09000000 (QEMU virt fixed
// mapping) with a FIXME(I-15).
//
// At P1-B: the base defaults to the QEMU virt fallback so early prints
// work before DTB parsing. After DTB parse, uart_set_base() updates it
// to the discovered address. Invariant I-15 (DTB-driven discovery) is
// satisfied for normal operation; the fallback exists only for the
// pre-DTB-parse window plus a recovery path if the DTB is corrupt.

#ifndef THYLACINE_ARCH_ARM64_UART_H
#define THYLACINE_ARCH_ARM64_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Update the PL011 base. Called by boot_main() after dtb_init() with
// the address returned by dtb_get_compat_reg("arm,pl011", ...).
//
// If never called, the default base remains 0x09000000 (QEMU virt
// fallback). Calling with 0 is a no-op (so callers can pass an
// uninitialized variable without harm).
void uart_set_base(uintptr_t base);

// P3-Bca: map the PL011 region [pa, pa+size) into the kernel vmalloc
// range and switch the active base to the returned kernel VA. Pre-
// P3-Bca the active base was the PA (accessed via TTBR0 identity); P3-Bd
// retires TTBR0 identity entirely so the vmalloc KVA is the durable
// form. boot_main() calls this exactly once after dtb_get_compat_reg
// returns the discovered PL011 reg.
//
// Extincts on mmu_map_mmio failure (vmalloc exhausted, etc.) — UART is
// load-bearing for boot diagnostics; silent fallback would mask the
// failure.
void uart_remap_to_vmalloc(uintptr_t pa, size_t size);

// Query the active base. Used by main.c to print it in the banner so a
// developer can confirm the DTB-driven discovery worked.
uintptr_t uart_get_base(void);

void uart_putc(char c);
void uart_puts(const char *s);

// #67 regression helper (test-only). Proves uart_putc BOUNDS its TXFF spin --
// it points the driver at a scratch region whose FR reads TX-full forever, IRQ-
// masks the swap, calls uart_putc, and returns true iff it terminated AND dropped
// the byte (a bounded, lossy TX beats an interrupt-dead CPU when the host serial
// consumer stalls). An unbounded spin would hang the boot inside this call.
bool uart_selftest_tx_bounded(void);

// A-4c-1: kernel UART console RX.
//
// PL011 interrupt number. On the QEMU virt machine the PL011 is wired to
// GIC SPI 1 = INTID 33. Hardcoded as the platform fallback (the same shape
// as the base-address fallback above): DTB `interrupts`-property parsing
// does not exist at v1.0 and is a Lazarus/portability follow-up. v1.0
// targets QEMU virt, so this is complete, not a half-version.
#define UART_INTID_PL011  33u

// Unmask the PL011 RX + RX-timeout interrupts (IMSC.RXIM | RTIM) after
// clearing any stale RX raw-interrupt state. boot_main() calls this once,
// then gic_attach(UART_INTID_PL011, uart_rx_handler, ...) + gic_enable_irq.
void uart_rx_init(void);

// True iff the PL011 RX path is live (CR.UARTEN + CR.RXE both set). QEMU's
// PL011 resets UARTEN clear, so this is false until uart_rx_init() runs --
// the #943 regression guard for "the console silently never receives".
bool uart_rx_path_enabled(void);

// PL011 RX IRQ handler (GIC dispatch target; runs in IRQ context). Drains
// the RX FIFO, splitting each 12-bit DR entry into a data byte + a break
// flag, and hands each to cons_rx_input. Clears the RX interrupts on exit.
void uart_rx_handler(uint32_t intid, void *arg);

// #174 backpressure resume. The cons reader calls this AFTER draining ring
// bytes (freeing space). If RX was paused (the ring filled and the handler
// masked RX, leaving bytes in the FIFO -- no loss), this drains the FIFO into
// the freed ring space and unmasks RX once the FIFO empties. A no-op (one
// atomic read) when not paused. Reader-driven resumption -- NEVER relies on the
// QEMU PL011 int_level re-fire (the #172 wedge trap). Runs in process context.
void uart_rx_pump(void);

// Print an unsigned 64-bit integer in hexadecimal, prefixed with "0x" and
// zero-padded to 16 hex digits (i.e. "0x0000000040080000"). Used for the
// boot banner's address fields.
void uart_puthex64(uint64_t v);

// Print an unsigned decimal. No padding, no negative values.
void uart_putdec(uint64_t v);

#endif // THYLACINE_ARCH_ARM64_UART_H
