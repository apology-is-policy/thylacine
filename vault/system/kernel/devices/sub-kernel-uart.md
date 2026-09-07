---
id: sub-kernel-uart
type: sub
parent: moc-kernel-devices
title: "The PL011 UART — the console's serial transport, and where the SAK break is seen"
code:
  - arch/arm64/uart.c
  - arch/arm64/uart.h
audit: hard
guarded-by: [inv-i9, inv-i15, inv-i27]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 23.5"
  - "docs/TRUSTED-PATH.md"
created: 2026-09-06
updated: 2026-09-06
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
target for the shared PL011 SPI), `uart_rx_pump` (the reader-side backpressure
resume), and `uart_rx_path_enabled` (the #943 regression predicate — RX is live
iff `UARTEN` and `RXE` are both set).

The base is set by `uart_set_base` (from `boot_main` after the DTB parse),
`uart_remap_to_vmalloc` (once the MMU maps MMIO), and read by `uart_get_base`.

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

### The RX drain: clear first, bound the loop, and never lose a byte

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
  `DR.BE` break flag, passed to `cons_rx_input` as the break bit — this is where
  the serial SAK's break condition enters the trusted path.
- **Backpressure without loss (#174).** When the console ring is full the drain
  **stops before reading `DR`**, leaving the byte in the FIFO (not lost), masks
  RX, and latches `g_rx_paused`. The FIFO then fills, QEMU's `can_receive` goes
  false, and the host buffers the overflow. `uart_rx_pump` resumes from the
  reader side once ring space frees.
- **The 1-byte holdback (#129).** `cons_rx_can_accept` is a lockless pre-check, so
  a second producer (the graphical keyboard's feed) can take the room between the
  check and the under-lock push — and by then the byte is already out of `DR` and
  cannot be put back. So the drain parks it in `g_rx_held_*` and re-offers it
  before touching the FIFO again, which is what makes a refusal lossless rather
  than merely narrowing the window. The one loop exit that could strand a held
  byte (`g_rx_held_valid && !g_rx_paused`) re-latches the pause (#136-F1),
  because a software-held byte raises no hardware interrupt and `uart_rx_pump`
  short-circuits on `!g_rx_paused` — it would otherwise vanish and reappear on
  the next unrelated keystroke.

### Publish-then-re-observe: a lost wake here is a dead console

`uart_rx_pause_and_recheck` masks RX and stores `g_rx_paused = true`, then
**re-reads** the room the pause was based on. Without that, a reader that drained
the ring between the drain's room-check and the store sees `false`, does nothing,
and parks on an empty ring — while the drain masks RX. The terminal state is
fatal, not slow: RX masked means the IRQ arm is gated off, `uart_rx_pump`'s only
caller is the parked reader, and on a serial-only console no other producer
exists — the console is dead until reboot, and the masked PL011 never raises
`DR.BE` again, putting the SAK out of reach too. This is the I-9
register-then-observe discipline, and it needs an explicit **StoreLoad fence**
(#136-F3): a release store followed by a relaxed load compiles to `stlrb` + `ldr`,
and ARMv8 orders `STLR→LDAR` but not `STLR→LDR`, so store-buffering could satisfy
the load before the store is observed. The same shape as the Weft-4 readiness
ring; a fence here keeps every other reader unperturbed.

### One SPI, dispatched on MIS; TX first

The PL011 RX, RX-timeout and TX sources share one interrupt line, so
`uart_irq_handler` reads the masked-interrupt-status register and services
whichever are pending. TX is serviced first — it is the cheap non-blocking arm,
and draining it promptly keeps a blocked writer's room-wait short — then RX under
its own lock and backpressure discipline.

## Data structures

None heap-owned. Module statics: `pl011_base` (volatile, documenting shared MMIO
state rather than needing the qualifier — the writer runs single-threaded before
IRQs); the backpressure latch `g_rx_paused` (acquire/release atomic); the 1-byte
holdback (`g_rx_held_valid` / `_byte` / `_break`, written only under the RX lock);
and two test-owned flags (`g_rx_test_armed`, `g_uart_tx_test_stalled`) that
production never sets. All PL011 register offsets are `#define`d against the
PrimeCell TRM.

## Concurrency

Two locks, and the split is load-bearing:

- **`g_uart_rx_lock`** serializes the RX FIFO drain, the RX IMSC mask/unmask, and
  `g_rx_paused` — so the IRQ handler (which pauses on a full ring) and the cons
  reader's `uart_rx_pump` (which resumes) never race on the FIFO or the mask.
- **`g_uart_imsc_lock`** is a pure **leaf** that serializes *every* IMSC
  read-modify-write. IMSC is one shared register: the RX path masks `RXIM|RTIM`
  under the RX lock, the TX path masks `TXIM` under the cons ring lock (a
  different outer lock), and two outer locks doing RMW on one register lose
  updates — a dropped `TXIM` re-arm is a silently wedged console, a dropped
  `RXIM` unmask is a dead keyboard. The leaf takes nothing and is held across two
  MMIO accesses, so nesting it inside either outer lock adds no cycle.

**Lock order: `g_uart_rx_lock` → `g_cons.lock`** (the RX drain calls
`cons_rx_input` under the RX lock). It is acyclic because the cons reader
*releases* `g_cons.lock` before calling `uart_rx_pump`, so there is no
`g_cons.lock → g_uart_rx_lock` edge. `g_rx_paused` is additionally an
acquire/release atomic so `uart_rx_pump`'s not-paused fast path needs no lock.

## Invariants enforced

**[[inv-i15]]** — the register base derives from the DTB; the QEMU-virt fallback
is the argued recovery-window exception.

**[[inv-i27]]** — the RX half of the trusted path. `DR.BE` break detection is
what feeds the serial SAK's attention condition into the console; the mint gate
on the `/dev/cons` path is [[sub-kernel-devdev]]'s complementary half.

**[[inv-i9]]** — the RX backpressure pause is a no-lost-wakeup site. A lost wake
between the reader and the drain deadlocks a dead console, so the pause publishes
then re-observes, behind a StoreLoad fence.

## Error paths

`uart_putc` drops the byte on the bounded-spin timeout (a stalled consumer is not
reading it). The RX drain returns false on backpressure (byte held in the FIFO or
in the 1-byte holdback, RX masked, `g_rx_paused` set) or a budget hit with the
FIFO non-empty — the handler ignores the distinction (a budget hit re-fires per
#172; a pause is resumed by the pump). `uart_remap_to_vmalloc` extincts on a NULL
mapping.

## Performance

The healthy TX path adds nothing — `TXFF` is clear on entry, the spin body never
runs, no timer is read. The RX drain is bounded at 64 iterations; #136-F4
re-derived the per-iteration cost after #129, which is higher than the old "well
under a timer tick" line implied (~320 MMIO worst case, each a vmexit under HVF)
but still sub-tick, and reachable only with a peer producer repeatedly taking the
room inside the window.

## Prosecution

- **The TX spin must stay bounded.** An unbounded spin goes interrupt-dead; the
  deadline plus the iteration backstop cover both the timer-up and pre-`timer_init`
  cases. `uart_selftest_tx_bounded` is the wiring guard, not just the arithmetic.
- **The RX drain must clear first and stay bounded.** Clearing after the drain
  strands a drain-window arrival (#172); an unbounded drain livelocks under a
  fast refiller (#172).
- **The backpressure pause must publish then re-observe, behind the fence.** A
  relaxed re-read, or none, reintroduces the dead-console lost-wake (#129-F1 /
  #136-F3).
- **The 1-byte holdback must re-offer before the FIFO and re-latch at the one
  loop exit.** A software-held byte raises no interrupt, so a strand is silent
  loss (#136-F1).
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
- **The test hooks** (`uart_test_tx_stall`, `uart_test_rx_force_hold` /
  `_held` / `_release_hold`, `uart_selftest_tx_bounded`) live in the production
  file for access to the static base and PL011 offsets — the same precedent as
  other test-only hooks in production files. The #133 leaked-state backstop keys
  on the test-owned `g_rx_test_armed`, never on the production `g_rx_paused` /
  `g_rx_held_valid`, so a human typing during the test phase neither reddens an
  innocent test nor loses a real keystroke (#136-F2).

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
