---
id: dec-2026-09-17-pci-interrupt-domains
type: dec
title: "Design function-bound shared INTx and MSI-X support"
date: 2026-09-17
status: standing
decided-by: user-vote
affects: [sub-kernel-irqfwd, sub-kernel-hwcap, sub-kernel-gic, sub-kernel-dtb, sub-kernel-discovery, sub-substrate-machine, sub-tapestryd, sub-nocturned, sub-libthyla-rs, sub-warden]
created: 2026-09-17
---
## Fork

Aux integration exposed a mismatch between exclusive IRQ claims and shared
PCI INTx routing. The operator asked that architectural bolt-ons be flagged.
A narrow per-function interrupt-disable operation and a full interrupt-domain
design were presented as alternatives.

## Research

The guest's level-line wait re-arms the GIC. The aux audio waiter deferred
acknowledgement to its cycle thread. Independently, Tapestry's polled input
leaves configuration notifications asserted. On the measured QEMU 10.0.2/HVF
machine keyboard slot 2 and audio slot 6 share INTID 37. QMP reported keyboard
ISR=3 and sound ISR=0; the audio waiter accumulated millions of dispatches.
Clearing the keyboard ISR in a diagnostic VM restored boot progress. This
host intervention is evidence, not a passing unmodified regression.

## Options

A narrow operation makes polled functions quiet, but leaves actual shared IRQ
users dependent on device order. Full function-bound endpoints support shared
INTx and kernel-mediated MSI-X, at the cost of new controller, lifetime and
mapping invariants.

## The call

The operator selected: "Design full shared IRQ / MSI-X support now" and
explicitly requested the Vault update. The proposed contract is
`docs/PCI-INTERRUPTS-DESIGN.md`. After reviewing that concrete design the operator explicitly answered
"Approve the full design and implementation". The ownership/ABI contract and
both MSI backend implementation stages are ratified; implementation remains
unfinished. Work stays single-agent under the existing operator direction.

## Rationale

Interrupt authority should follow the owned PCI function, not the incidental
wire it shares with other functions. A complete design exposes the MSI-X table
mapping and late-interrupt teardown obligations before implementation.
