---
id: sub-kernel-uart
type: sub
parent: moc-kernel-devices
title: "The PL011 UART — the console's serial transport, and where the SAK break is seen"
code:
  - arch/arm64/uart.c
  - arch/arm64/uart.h
audit: hard
guarded-by: [inv-i15, inv-i27]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 23.5"
  - "docs/TRUSTED-PATH.md"
created: 2026-09-06
updated: 2026-11-09
---
## Purpose

The PL011 serial UART driver — the kernel's own transport for the serial
console. It is three paths sharing one hardware line: a **polled TX** that works
from the earliest boot (before interrupts, before the scheduler), an
**IRQ-driven RX** that is the receive half of the I-27 trusted path (it is where
the SAK's break condition is seen), and an **IRQ-driven TX** that the console
ring drains through. The register base is discovered from the device tree
(I-15), with a QEMU-virt fallback used only in the window between kernel entry
and the DTB parse.

It sits under [[moc-kernel-devices]] — hardware the kernel drives itself rather
than lending to a userspace driver — but its closest neighbours are in the
console area: it feeds bytes into [[sub-kernel-cons]] and its RX gate is one half
of the trusted path [[sub-kernel-devdev]] gates the other half of.

## Contract

The direct print path is `uart_putc` / `uart_puts` / `uart_puthex64` /
`uart_putdec` — bounded, always safe to call, used by the extinction/Halls dump
and every early-boot print. `uart_puts` translates `\n` to `\r\n` for a real
terminal.

The ring-drained TX path is `uart_tx_try_putc` (a **non-blocking** single-byte
FIFO push, safe under a lock and from IRQ context), armed and cleared by
`uart_tx_irq_set_enabled` / `uart_tx_irq_clear`, drained synchronously before a
crash dump by `uart_tx_drain_sync`, and probed empty by `uart_tx_fifo_empty`.

The RX path is `uart_rx_init` (bring-up), `uart_irq_handler` (the GIC dispatch
target for the shared PL011 SPI), and `uart_rx_path_enabled` (tests build only; the #943 regression predicate — RX is live
iff `UARTEN` and `RXE` are both set).

The base starts at the QEMU virt fallback (`0x09000000`), is set by
`uart_remap_to_vmalloc` (from `boot_main` after the DTB parse, once the MMU
maps MMIO), and read by `uart_get_base`.

## Mechanism

### The base is DTB-driven, with one argued fallback

`pl011_base` starts at the QEMU-virt fallback `0x09000000` so prints work during
the earliest boot, and `boot_main` updates it to the DTB-discovered `arm,pl011`
address. I-15 is satisfied for normal operation; the fallback is a recovery path
between kernel entry and `dtb_init`, the documented exception the invariant
carries. `uart_remap_to_vmalloc` later swings the base to a mapped MMIO virtual
address; it extincts rather than run with a NULL mapping.

### TX is bounded, because the live path was never replaced

`uart_putc` spins on `TXFF` (the TX FIFO full flag), and that spin is
**bounded** — a wall-clock deadline (20 ms, once the timer is up) plus an
unconditional iteration backstop (~16.7M FR reads). The reason is #67: a stalled
host serial consumer leaves the FIFO full indefinitely, and the original
unbounded spin "fine at P1-B (single CPU, no scheduler)" became a soundness
hazard when P1-F (a buffered IRQ TX) was never built and this stayed the live TX
path — an unbounded spin goes interrupt-dead (no timer tick, no IPI), and inside
an IRQ dispatch it manufactures a seconds-long INTID stall. On timeout it
**drops the byte**: a bounded-but-lossy console is strictly sounder than a
wedged CPU, and the Halls crash dump (which runs IRQ-masked on a dying machine)
depends on this. The healthy path never enters the loop body and never reads the
timer — the `t0` anchor is set lazily on the first spin, so a frozen `CNTVCT`
pre-`timer_init` is covered by the iteration backstop. `uart_selftest_tx_bounded`
revert-probes the bound: it points the base at a scratch region whose FR reads
full forever and confirms `uart_putc` returns having dropped the byte.

### The RX drain: clear first, bound the loop, read every byte

`uart_rx_drain_locked` (under `g_uart_rx_lock`) is the shared RX core.

- **Clear-first (#172).** It clears the RX + RX-timeout interrupts *before*
  draining. QEMU's PL011 RX interrupt is receive-driven and not recomputed from
  the FIFO level on an ICR clear, so clearing after the drain would strand a byte
  arriving in the post-drain window and wedge the FIFO.
- **Bounded (#172).** The drain is capped at `UART_RX_DRAIN_MAX = 64` (four FIFO
  depths). An unbounded `while (!RXFE)` livelocked the CPU under sustained input,
  because QEMU refills the FIFO as fast as the guest drains it — especially under
  HVF, where the main loop tops it up on a parallel host thread — so `RXFE` never
  became true and the handler never returned (IRQs masked → whole-OS freeze,
  reproduced holding an arrow key).
- **The break is the SAK's (I-27).** Each byte read from `DR` carries the
  `DR.BE` break flag, passed to `cons_rx_receive` as the break bit — this is where
  the serial SAK's break condition enters the trusted path.
- **Every byte is read (vote 73, 2026-11-05).** The drain hands each byte to
  `cons_rx_receive`, which never refuses, and RX is never masked for
  back-pressure. What the console's line discipline cannot take waits in its
  bounded side queue ([[sub-kernel-cons]]), so a BREAK or a Ctrl-C behind a full
  ring is read on arrival whether or not anyone reads the console. QEMU's PL011
  takes a break into the FIFO whatever its depth, and masking only gates the
  IRQ, so a masked drain left the SAK unread for as long as no reader came.
  Vote 73 retired the #174 pause, the #129 one-byte holdback and the
  publish-then-re-observe recheck that guarded them
  ([[chg-2026-11-09-serial-side-queue]]). The cost is the queue's bound: a flood
  past ring and queue drops its newest bytes, counted, where the pause held
  them in the host.

### One SPI, dispatched on MIS; TX first

The PL011 RX, RX-timeout and TX sources share one interrupt line, so
`uart_irq_handler` reads the masked-interrupt-status register and services
whichever are pending. TX is serviced first — it is the cheap non-blocking arm,
and draining it promptly keeps a blocked writer's room-wait short — then RX under
its own lock.

## Data structures

None heap-owned. Module statics: `pl011_base` (volatile, documenting shared MMIO
state rather than needing the qualifier — the writer runs single-threaded before
IRQs), and one test-owned flag (`g_uart_tx_test_stalled`) that production never
sets. All PL011 register offsets are `#define`d against the
PrimeCell TRM.

## Concurrency

Two locks, and the split is load-bearing:

- **`g_uart_rx_lock`** serializes the RX FIFO drain.
- **`g_uart_imsc_lock`** is a pure **leaf** that serializes *every* IMSC
  read-modify-write. IMSC is one shared register: the RX bring-up unmasks
  `RXIM|RTIM`, the TX path masks `TXIM` under the cons ring lock (a different
  outer lock), and two outer locks doing RMW on one register lose
  updates — a dropped `TXIM` re-arm is a silently wedged console, a dropped
  `RXIM` unmask is a dead keyboard. The leaf takes nothing and is held across two
  MMIO accesses, so nesting it inside either outer lock adds no cycle.

**Lock order: `g_uart_rx_lock` → `g_cons.lock`** (the RX drain calls
`cons_rx_receive` under the RX lock). Nothing holding `g_cons.lock` takes the RX
lock, so the order is acyclic.

## Invariants enforced

**[[inv-i15]]** — the register base derives from the DTB; the QEMU-virt fallback
is the argued recovery-window exception.

**[[inv-i27]]** — the RX half of the trusted path. `DR.BE` break detection is
what feeds the serial SAK's attention condition into the console; the mint gate
on the `/dev/cons` path is [[sub-kernel-devdev]]'s complementary half. Since
vote 73 the drain reads every byte, so no state of the console ring or of its
reader keeps a break from reaching the console.

## Error paths

`uart_putc` drops the byte on the bounded-spin timeout (a stalled consumer is not
reading it). The RX drain stops at its budget with the FIFO non-empty and leaves
the rest to the re-fire (#172); it has no failure return, since the console never
refuses a byte. `uart_remap_to_vmalloc` extincts on a NULL
mapping.

## Performance

The healthy TX path adds nothing — `TXFF` is clear on entry, the spin body never
runs, no timer is read. The RX drain is bounded at 64 iterations of two
MMIO (FR, DR) and one `cons_rx_receive` under the console lock; nothing in the
loop masks or unmasks.

## Prosecution

- **The TX spin must stay bounded.** An unbounded spin goes interrupt-dead; the
  deadline plus the iteration backstop cover both the timer-up and pre-`timer_init`
  cases. `uart_selftest_tx_bounded` is the wiring guard, not just the arithmetic.
- **The RX drain must clear first and stay bounded.** Clearing after the drain
  strands a drain-window arrival (#172); an unbounded drain livelocks under a
  fast refiller (#172).
- **The drain must never leave a byte in the FIFO for back-pressure.** A byte
  left there keeps everything behind it unread, a break included; what the
  console cannot take belongs in its side queue (vote 73).
- **Every IMSC write must go through the leaf lock.** Two outer locks RMW-ing one
  register lose a `TXIM` or `RXIM` update — a wedged console or a dead keyboard.
- **`DR.BE` must keep reaching the console as the break bit**, or the serial SAK
  is unreachable.
- **The base must stay DTB-driven** past the boot fallback window.

## Seams

- **P1-F (a fully buffered IRQ TX) was never built**, so `uart_putc`'s bounded
  spin is the live direct TX path rather than a compatibility shim. The ring
  drain (`uart_tx_try_putc` + the cons TX ring) is the buffered path that did
  land.
- **The test hooks** (`uart_test_tx_stall`, `uart_selftest_tx_bounded`) live in
  the production file for access to the static base and PL011 offsets — the same
  precedent as other test-only hooks in production files.

## Caveats

- **`pl011_base` is `volatile` as documentation, not necessity.** The writer is
  single-threaded and runs before interrupts, but the qualifier marks the shared
  MMIO state and prevents caching across calls.
- **The RX SPI is routed to cpu0** (the GIC target), which `uart_selftest_tx_bounded`
  relies on when it swaps the base under an IRQ mask — widen its scratch array or
  re-check the affinity before routing the SPI elsewhere or adding a `>0x3c` UART
  access reachable during the swap.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)

**Console gate append (2026-10-14).** `uart_putc` is the gated wrapper over
`uart_putc_wire`: after a peer's extinction claims the console, this CPU's
bytes are dropped so the banner reaches the wire whole (sub-kernel-halls).
`uart_tx_try_putc` (the ring drain) stays ungated: it runs under the ring lock
the winner holds.

`uart_putc` passes enter's token to exit: with IRQs on the byte holds its
thread's preempt count, and with IRQs masked it touches no thread state
(sub-kernel-halls, round 3).
