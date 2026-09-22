# Graphical SAK portability contract

Research and design refinement, 18 September 2026. The operator approved the
isolated trusted display/input service and requires portability to Pi 400,
Pi 500 and future output hardware. This records a contract, not a claim that
those bare-metal backends already work. Graphical authorization remains disabled
until the implementation satisfies the contract on the selected backend.

## What the hardware evidence changes

| Target | Evidence | Consequence for Thylacine |
| --- | --- | --- |
| Pi 400 | Its board DTS identifies `raspberrypi,400`, BCM2711, and inherits Pi 4 configuration. | Discover the actual boot DTB and board revision; do not hard-code QEMU addresses or assume Pi 4 connector wiring. |
| Pi 500 | Its DTS identifies `raspberrypi,500`, BCM2712, and inherits the D0 Pi 5 configuration. | Use the board's translated resources and compatible strings; Pi 400 register layouts are not a Pi 500 backend. |
| Broadcom display | Linux documents separate HVS composition, planes and pixel-valve timing. BCM2712 describes its own pixel-valve blocks. | A protected linear buffer does not establish exclusive output. Own all display lists, planes, cursors, output selection and capture paths. |
| RP1 input | RP1 has two xHCI controllers and aggregates peripherals behind a single PCI function. | Device-function ownership alone is too coarse for the input trust boundary. Bus ownership and child-controller resource authority must compose. |
| BCM2712 DMA | Raspberry Pi's https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/drivers/iommu/bcm2712-iommu.c driver describes shared clients and an address region that bypasses translation. | An https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/drivers/iommu/bcm2712-iommu.c node is not evidence of complete isolation. Audit each DMA master's reachable addresses and bypass configuration. |

Primary sources: [Pi 400 DTS](https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/arch/arm/boot/dts/broadcom/bcm2711-rpi-400.dts), [Pi 500 DTS](https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/arch/arm64/boot/dts/broadcom/bcm2712-rpi-500.dts),
[BCM2712 DTS](https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/arch/arm64/boot/dts/broadcom/bcm2712.dtsi), [Linux display documentation](https://docs.kernel.org/gpu/vc4.html),
[RP1 peripherals, chapters 1, 2, 5 and 6](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf),
[BCM2712 https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/drivers/iommu/bcm2712-iommu.c implementation](https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/drivers/iommu/bcm2712-iommu.c).
The source links pin Raspberry Pi Linux to `9c40c75f681b0b8882db9b624dfec8f81ff492f2`;
these are implementation evidence, not a hardware-security certification.

## Portable authority, separate backends

The trusted role is a logical seat: its presentation domain and admitted physical
input sources. It is not a PCI function, a framebuffer address, or a virtio device
number. Kernel boot policy binds the trusted role and its resource graph. A DTB
can describe resources but cannot self-authorize an arbitrary process as trusted.
Boot firmware and configuration integrity remain part of the assumed trusted base.

The initial service can host both display and input backends. Keep their interfaces
separate: GPU transport, display controller and USB host are not necessarily one
physical device. Use the MENAGERIE bus/resource hierarchy for platform MMIO and
PCI children. Do not give the renderer an entire RP1 BAR merely because a keyboard
controller lives within it. Shared registers, resets, clocks, IRQ domains and DMA
translation controls stay with their designated trusted owner. A child grant must
not let one controller reset or remap another. Until those boundaries exist, the
necessary bus owner is explicitly in the TCB; it is never described as isolated.

Corvus owns request identity, policy, verification, rate limits and grants. The
trusted service owns bounded presentation and exclusive input routing. Tapestry
owns ordinary composition. Backends translate authority into hardware operations;
neither Corvus nor the kernel episode state machine depends on virtqueues, PCI
MSI addresses, HDMI registers, USB report layouts or CPU cache coherence.

## Entry is a hardware acknowledgement, not a paint request

A generation moves through normal, quiescing, exclusive, restoring and failed
states. Secret input and authorization are forbidden before exclusive state.
The service acknowledgement covers all of the following, with a bounded deadline:

1. Stop admitting ordinary presentation, cursor changes, modesets and input
   delivery. Drain or safely retire earlier work; a submitted fence is not a
   completed display transition. No unbounded GPU wait in the authentication path.
2. Take control of every plane and visible output in the seat. Display the trusted
   scene on the chosen output and trusted neutral content on other outputs. Do not
   leave an ordinary second monitor presenting a competing authorization scene.
   Disable or mediate writeback, display capture and screenshot exports.
3. Establish that the trusted frame is actually selected for scanout, including
   backend-specific latch/vblank and cache synchronization requirements. Prove
   this from documented completion semantics, not a sleep or successful submit.
4. Route only admitted physical input to the episode. Drop pre-entry events and
   require the attention chord to be released before accepting secret keystrokes.
   USB enumeration, controller queues and HID decoding on this path are TCB.

A GPU command fence may prove command completion without proving physical scanout.
Each backend must state exactly what its acknowledgement proves. QEMU is the first
backend, not the definition of that acknowledgement. An unprovable transition
fails closed rather than showing an ordinary modal and accepting a password.

The blurred backdrop is optional. Copy a completed normal image to private backing
with no surviving untrusted write alias; account for overlays or use a neutral
field. Never read back trusted pixels into a client-visible buffer. Trusted backing
is excluded from ordinary resource IDs, GPU contexts, exports, DMA and screenshots.

## DMA and acceleration are explicit trust obligations

For every enabled DMA master, record its owner, address aperture, translation,
bypass paths and access to trusted input, Corvus memory and display backing.
CPU page permissions and a GPU-private MMU are not a system-wide DMA barrier.
The BCM2712 driver is a concrete warning against inferring isolation from a device
named https://github.com/raspberrypi/linux/blob/9c40c75f681b0b8882db9b624dfec8f81ff492f2/drivers/iommu/bcm2712-iommu.c; it is not evidence that the hardware cannot be configured securely.

Where hardware isolation is absent or unproven, DMA-programming drivers and their
command validators belong to the TCB. No ordinary client may supply arbitrary
physical addresses or unvalidated commands that reach protected resources. Disable
an unsafe untrusted device/acceleration path rather than silently claiming that it
is contained. Boot-loaded trusted drivers may continue networking/audio under the
explicit existing trust assumption; this is not protection from their compromise.

Normal GPU contexts may survive an episode only if their ongoing execution cannot
modify/read trusted resources, alter scanout, or prevent bounded takeover. Otherwise
submission must be quiesced or the acceleration mode rejected for trusted episodes.
Resetting a device is failure recovery, not the normal workspace-preserving design.
Cache clean/invalidate, DMA ownership transfers, memory attributes, MMIO barriers,
interrupt acknowledgement and fences are backend duties, tested independently of
QEMU's memory behavior. Never reuse DMA backing before retirement completes.

## Input, failure and recovery

The reserved gesture is recognized from a kernel-bound physical-source route before
ordinary delivery. Software injection has a distinct endpoint and cannot assert
physical provenance. Source identifiers carry lifetimes/generations; unplug and
reuse cannot turn a stale queue into a trusted keyboard. No input hotplug admission
during an episode without reestablishing the seat. Device firmware, malicious USB
hardware, physical tampering and the VM host remain trust assumptions; a USB key
report does not cryptographically prove a human pressed a key.

Output hotplug, suspend/resume, reset, mode/topology change, trusted-owner death,
input-owner loss and acknowledgement timeout invalidate the episode. Cancel pending
authorization, flush secret/held-key state and revoke the old generation before
recovery. Discard stale completions and old END messages. Never replay Enter/Escape.
A service failure need not successfully blank the screen: stale visible pixels are
possible, but cannot remain an authorized input or grant channel.

Production retains its graphical-only authorization posture. No automatic serial
fallback. Serial recovery is available only in a separately selected dev/recovery
posture; it cannot be enabled by a failed graphical service. A userspace trusted
renderer does not automatically solve kernel panic rendering: a crash sink needs
its own ownership and failure contract, without a second live hardware owner.

The image of the dialog is reproducible by ordinary software outside an episode.
The security anchor is the user's attention gesture followed by exclusive routing,
not an intrinsically uncopyable colour, logo or pixel band.

## Evidence required before enabling a backend

The same conformance suite must run with a hostile normal presentation client:
late flips, cursors/overlays, forged resources, active accelerated work, delayed
completions, screenshot requests, input injection, held keys and service death.
Test a second output, hotplug, modesets, failed cache/fence transitions, DMA alias
attempts, bounded failure and restoration with complete repaint. Check audio,
Haul and Gallery continuity. Keep the existing serial authorization regression.

For Pi 400 and Pi 500 independently, record boot firmware/DTB revisions, controller
resources and DMA trust inventory; trace the actual built-in keyboard route rather
than assuming its host port from a family name. Test hardware takeover with both
HDMI outputs and real input. Check power/reset/unplug and delayed DMA completion.
QEMU screenshots, host simulation and a Pi Linux source audit cannot discharge
these physical-hardware gates. No bare-metal security claim is made by this note.

Still to implement: the trusted hardware broker, kernel-bound episode protocol,
Corvus presentation integration, backend conformance tests and actual graphical
screenshots. Pi controller drivers and hardware qualification remain separate
bring-up work; the common interface must make their obligations expressible now.
