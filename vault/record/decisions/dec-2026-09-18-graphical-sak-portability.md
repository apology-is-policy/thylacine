---
id: dec-2026-09-18-graphical-sak-portability
type: dec
title: "Approve isolated graphical SAK service with portable hardware contract"
date: 2026-09-18
status: standing
decided-by: user-vote
affects: [sub-corvus, sub-tapestryd]
created: 2026-09-18
---
## Fork

The earlier kernel-only graphical sink conflicted with userspace GPU ownership.
The proposed persistent trusted display/input service avoids reset-based workspace
loss; the alternative puts hardware mediation and graphics parsing in the kernel.

## The call

The operator approved the proposed graphical design and explicitly required
research and secure portability, with Pi 400 and Pi 500 the immediate bare-metal
targets. The service is approved; no additional TCB ratification is pending.
Work remains single-agent. This decision does not claim runtime implementation.

## Research and consequences

`docs/GRAPHICAL-SAK-PORTABILITY.md` pins primary Raspberry Pi Linux sources and
links official RP1 and Linux display documentation. HVS planes/output state extend
beyond one framebuffer; RP1 aggregates controllers within one PCI function; the
BCM2712 IOMMU implementation permits a bypass region. Consequently the common
contract covers the whole seat, controller-level resources, DMA trust inventory,
backend-specific completion evidence and failure without serial policy downgrade.

`docs/GRAPHICAL-SAK-OWNERSHIP.md` is the approved ownership design. Corvus retains
authentication and Imperium policy. Tapestry loses physical hardware claims only
when the broker implementation lands. The source audit is not hardware validation.

## Alternatives and residuals

A Halcyon modal, second QEMU GPU or reset-per-episode cannot establish the intended
portable, workspace-preserving boundary. Trusted userspace can observe keys and
control pixels; non-isolated DMA drivers and firmware remain explicit TCB. A
compromised trusted input driver can fabricate key reports. Physical-device and
VM-host compromise are not excluded by a software gesture. Production cannot
silently enable serial input when graphical authorization fails.
