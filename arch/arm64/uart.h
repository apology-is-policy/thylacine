// PL011 UART driver — minimal polled I/O for early boot.
//
// Used by boot_main() and any panic path that needs to print before the
// real device infrastructure is up. Replaced by /dev/cons (kernel Dev,
// per ARCH §9.4) at Phase 3.
//
// PL011 base address is currently hardcoded for QEMU virt (0x09000000).
// This is a P1-A shortcut — invariant I-15 (DTB-driven discovery) is
// violated for one chunk only. P1-B parses the DTB and replaces the
// hardcoded base with the discovered address. See `docs/phase1-status.md`
// trip-hazard list.

#ifndef THYLACINE_ARCH_ARM64_UART_H
#define THYLACINE_ARCH_ARM64_UART_H

#include <stdint.h>

void uart_putc(char c);
void uart_puts(const char *s);

// Print an unsigned 64-bit integer in hexadecimal, prefixed with "0x" and
// zero-padded to 16 hex digits (i.e. "0x0000000040080000"). Used for the
// boot banner's address fields.
void uart_puthex64(uint64_t v);

// Print an unsigned decimal. No padding, no negative values.
void uart_putdec(uint64_t v);

#endif // THYLACINE_ARCH_ARM64_UART_H
