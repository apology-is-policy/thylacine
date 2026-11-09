---
id: chg-2026-11-09-serial-side-queue
type: chg
title: "The serial console reads every byte; what the ring refuses waits in a side queue (vote 73)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-cons
  - sub-kernel-uart
  - sub-kernel-devctl
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-09
---
Operator vote 73 ([[dec-2026-11-05-serial-side-queue]]). `uart_rx_drain_locked` hands every FIFO byte to the new `cons_rx_receive`, which never refuses, and RX stays unmasked from bring-up on: `g_rx_paused`, the #129 holdback (`g_rx_held_*`), `uart_rx_pause_and_recheck`, `uart_rx_pump`, `uart_rx_set_enabled`, `cons_rx_can_accept` and the test hooks that seeded them are deleted, with the kernel tests `cons.rx_can_accept_boundary`, `cons.rx_holdback_reoffer` and `cons.rx_holdback_not_stranded` and the runner's `uart-rx-hold` sweep bit. In kernel/cons.c the line discipline moves into `cons_rx_take_locked` (under the console lock, its echo and wakes staged in a `cons_rx_fx`), shared by the refusable `cons_rx_input` (feed, tests) and the queue. `cons_rx_receive`: a BREAK is taken at once; a byte with nothing queued goes to the discipline; otherwise, or if refused, `cons_side_offer_locked` queues it (4096 u16 entries), an ISIG Ctrl-C posting its interrupt at once and queued as SIGNALLED so its line discard happens in order; full, it drops the newest (`rx_drop_side`), a signalled entry displacing the newest data entry. The reader moves the queue after every drain, sixteen entries per lock hold; BEGIN and `cons_test_reset` empty it. `/ctl/cons` gains `rx_drop_side`, the INPUT DROP line `side=`. Kernel tests: `cons.rx_side_sak_and_intr_behind_full_ring`, `cons.rx_side_cooked_intr_in_order`, `cons.rx_side_cooked_typing_past_half`, `cons.rx_side_full_drops_and_counts`, `cons.rx_side_discarded_at_begin`. Scenario `ls-serial-sak-typeahead` (Ctrl-C and BREAK behind 80 unread lines into `sleep 60`; every line then runs in order, or none after the episode); its fake-guest rows (work/nov03-mm1/v73) read 11 of 11 as designed. The holdback's history (its strand at one budget exit, #136-F1, and the grading lesson) left [[sub-kernel-cons]] with it; it stays in [[chg-2026-08-16-cons-writer-set]].
