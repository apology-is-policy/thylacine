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

#include <stdint.h>

// Update the PL011 base. Called by boot_main() after dtb_init() with
// the address returned by dtb_get_compat_reg("arm,pl011", ...).
//
// If never called, the default base remains 0x09000000 (QEMU virt
// fallback). Calling with 0 is a no-op (so callers can pass an
// uninitialized variable without harm).
void uart_set_base(uintptr_t base);

// Query the active base. Used by main.c to print it in the banner so a
// developer can confirm the DTB-driven discovery worked.
uintptr_t uart_get_base(void);

void uart_putc(char c);
void uart_puts(const char *s);

// Print an unsigned 64-bit integer in hexadecimal, prefixed with "0x" and
// zero-padded to 16 hex digits (i.e. "0x0000000040080000"). Used for the
// boot banner's address fields.
void uart_puthex64(uint64_t v);

// Print an unsigned decimal. No padding, no negative values.
void uart_putdec(uint64_t v);

#endif // THYLACINE_ARCH_ARM64_UART_H
