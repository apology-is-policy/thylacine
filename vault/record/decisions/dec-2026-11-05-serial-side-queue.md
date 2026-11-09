---
id: dec-2026-11-05-serial-side-queue
type: dec
title: "The serial drain reads every byte into a bounded side queue, so BREAK and Ctrl-C act behind a full ring"
date: 2026-11-05
status: standing
decided-by: user-vote
affects: [sub-kernel-cons, sub-kernel-uart]
created: 2026-11-09
---
## Fork

Audit i32 round 1 (F4, P2, inherited, read in the code) found that the serial console paused receive once its ring could no longer reserve a whole cooked line (more than 255 bytes unread): the PL011 drain left the next byte in the FIFO and masked RX, and only the console reader's pump resumed it. An ordinary foreground child reads no console, so type-ahead into it left a Ctrl-C and a BREAK unread for as long as it ran; the kernel test that claimed a full ring never blocks the SAK proved the line discipline's acceptance, not the path to it. QEMU's PL011 takes a break into the FIFO whatever its depth, and masking only gates the IRQ. Vote 50's audit added that one cooked line read with more than 255 bytes left behind keeps receive paused.

## Decision

Operator vote 73: the serial drain keeps draining the UART into a bounded side queue, honouring BREAK and ISIG as read; the queue holds input losslessly until it fills, then the newest byte is dropped and counted. The alternatives were dropping only after about a second without a reader (a timer in the pump), and leaving it documented that the serial SAK needs a reader or under 256 bytes of type-ahead. Heritage: Linux's 8250 reads RX regardless of the flip buffer and runs the SAK from the break in the interrupt path; tty_buffer drops past its memory limit.

## Consequences

A paste stays lossless up to ring plus queue (512 + 4096 bytes); past that the serial console drops, counted at `/ctl/cons` as `rx_drop_side` and in the one-shot INPUT DROP line, where the old pause held a flood in the host without limit. The #174 pause, the #129 one-byte holdback and its recheck are gone. A queued Ctrl-C posts its interrupt on arrival and discards in order. BEGIN discards the queue with the ring, which closes the residue IMPERIUM-DESIGN 11 item 5 had documented (a held byte pumped into the trusted reader's first drain).
