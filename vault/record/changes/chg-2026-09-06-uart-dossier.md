---
id: chg-2026-09-06-uart-dossier
type: chg
title: "author sub-kernel-uart over the arch/arm64/uart.c orphan -- the PL011 driver + the I-27 RX break/SAK half + the I-9 backpressure pause; audit:hard under moc-kernel-devices (1 of the 2 remaining orphans resolved)"
date: 2026-09-06
arc: arc-vault
commits: []
touched: []
established: [sub-kernel-uart]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
`arch/arm64/uart.c` was UNOWNED -- an orphan flagged across two prior Explores
(it blocks stubbing 01-boot + 31-trivial-devs, whose console/uart content it
holds). Authored a dedicated dossier rather than deferring, per the
chunk-completeness default; it is the "A-4c trusted path: kernel console RX +
SAK" audit-trigger surface, so audit:hard.

sub-kernel-uart (parent moc-kernel-devices, code: arch/arm64/uart.{c,h},
guarded-by [inv-i9, inv-i15, inv-i27]) written from a full read of uart.c:
- I-15: the DTB-driven base with the argued QEMU-virt boot-window fallback.
- I-27: the RX half of the trusted path -- DR.BE break detection feeds the serial
  SAK's attention condition into cons; devdev gates the /dev/cons mint half.
- I-9: the #174 backpressure pause is a no-lost-wake site (a lost wake deadlocks
  a dead console + puts the SAK out of reach), so publish-then-re-observe behind
  the #136-F3 StoreLoad fence (STLR->LDR unordered on ARMv8; the Weft-4 shape).
- the #67 bounded TX spin (drop-on-timeout sounder than a wedged CPU;
  uart_selftest_tx_bounded revert-probe), the #172 clear-first + bounded RX
  drain, the #129 1-byte holdback + #136-F1 restore-at-the-one-exit, the two-lock
  split (g_uart_rx_lock + the g_uart_imsc_lock LEAF serializing all IMSC RMW --
  the #75 shared-register hazard) with the acyclic g_uart_rx_lock -> g_cons.lock
  order.

This resolves 1 of the 2 remaining orphans (joey.c=boot-mounts is the other,
owned in part by sub-stratum-boot). No code touched; no audit owed (a dossier
over existing code). The 01-boot / 31-trivial-devs stubs that will redirect here
follow once the entry/trivial-devices Explore's breadth map lands.
