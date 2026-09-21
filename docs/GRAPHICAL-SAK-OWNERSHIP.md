# Graphical Lex curiata: hardware ownership review

Approved by the operator, 18 September 2026, with an explicit portability
requirement for Pi 400, Pi 500 and future graphical output. Implementation is in progress in `usr/lictor`; runtime qualification is pending. [Portability contract and research](GRAPHICAL-SAK-PORTABILITY.md)
refine the backend obligations below.

## Approved decision

Use an isolated, boot-trusted display/input service with portable display and input backends,
with kernel-bound episode authority. Extract hardware ownership from Tapestry;
keep Halcyon, Beacon, layout, themes and ordinary window composition outside this
trusted service. Corvus retains authentication and all Imperium policy.

This explicitly revises TRUSTED-PATH section 7's requirement that the kernel
rasterize all trusted graphics. It preserves ARCHITECTURE section 17.2's userspace
graphics policy. It does enlarge the trusted userspace base beyond Corvus: the
hardware service can observe keys and control pixels. That residual is real and
must be documented, audited and tested, rather than described as kernel-owned
pixels. No ordinary compositor process gets that authority.

## Why this decision is needed

Before this separation, tapestryd owned the virtio GPU and keyboard PCI functions, their BAR mappings,
DMA queues and IRQ endpoints. Its gpu.rs also implemented accelerated resources,
contexts and presentation lifetimes. A full-screen Halcyon dialog would still be
paintable and readable by that ordinary compositor. Freezing its UI does not revoke
its hardware mappings or outstanding DMA.

Temporarily resetting/reclaiming the GPU would destroy live accelerated contexts
and resources. A reset-only solution would not preserve the current Gallery/game
workspace. Copying userspace virtqueue state into a kernel driver would add an
untrusted command parser and a second owner to a live device. Neither is a sound
small patch.

The persistent hardware owner avoids changing the GPU owner at each SAK.
The episode changes which principal may supply display/input content to that owner.
The separation is architectural work, not merely a modal overlay.

## Ownership and interfaces

- The boot manifest identifies one trusted hardware service. It alone claims the
  presentation and input resources (platform controllers or bus children, not
  necessarily PCI functions). Tapestry loses those claims and
  raw BAR/DMA access. Device ownership is never granted by a runtime self-assertion.
- Warden starts the hardware service and DMA-only compositor as separate narrowed
  leaves. The existing no-child rule for narrowed drivers remains intact. A
  boot-stamped normal-client designation admits only that process incarnation to
  the broker and confers no trusted endpoint authority.
- The kernel binds this service instance and Corvus to a generation-bearing episode.
  Registration and control handles are non-transferable, non-inheritable and
  invalid after owner death. Generic CAP_HW_CREATE is not enough to register as
  the trusted display owner.
- Normal surface/resource operations cross a bounded broker interface. No raw
  virtqueue descriptors, physical addresses, scanout commands, trusted resource IDs
  or unrestricted command passthrough are accepted from Tapestry or applications.
  Accelerated context/resource ownership and fences remain explicit and tested.
- The trusted service retains the GPU transport/resource machinery needed for
  existing acceleration. It is therefore larger than a framebuffer blitter; moving
  gpu.rs wholesale would not by itself complete the security separation. The new
  boundary must validate resource ownership and prevent application commands from
  referring to trusted scanout/backing resources.
- Corvus submits bounded semantic content (principal, requested capabilities, term,
  state, masked input length) through the episode-bound endpoint. Only the bound
  Corvus instance can submit or finish the current episode. No secret is sent to
  Tapestry, Halcyon, Beacon, a PTY or an ordinary surface.
- The trusted service uses fixed baked fonts, palette and framing. It does not load
  user themes, images, font files, configuration or arbitrary UI programs.

## Entry and restoration

1. A physical reserved attention gesture reaches the trusted input owner. Software
   input injection is a distinct untrusted route and cannot trigger SAK or enter
   the secret queue. Serial BREAK remains a recovery trigger only in a configured dev/recovery posture.
2. The kernel starts a generation and freezes normal presentation/input admission.
   The trusted owner drains previously admitted presentation fences before acknowledging
   exclusive trusted scanout. Corvus cannot accept secret input before this acknowledgement.
3. The last completed frame is copied into private immutable trusted backing,
   blurred once and dimmed. If a safe copy is unavailable, use the approved neutral
   field. Cursor updates and outstanding normal scanout work are also excluded.
4. Corvus's exact immutable request is shown in the approved Lex curiata dialog.
   Key bytes go only to Corvus. Wrong-key counters, eligibility, request expiry,
   requester revalidation and grants use the existing Imperium implementation.
5. Finish/cancel clears secret input, drops held keys and releases, invalidates
   the episode generation, then restores normal admission. Tapestry repaints fully;
   it does not assume old dirty rectangles remain valid. Halcyon retains its layout
   and restores focus only to a still-live surface.

Audio, networking and remote mounts continue. Ordinary applications may continue
running and updating their private surfaces; none of those updates reach scanout
or the immutable episode background until restoration.

## Failure rules

A dead/stalled trusted service never causes the compositor to inherit display or
input ownership. Cancel the pending grant and invalidate the generation. Serial
recovery is available only under the configured dev/recovery posture; production
does not enable serial authorization on graphical failure. Recovery/restart must establish exclusive hardware ownership again before
showing a new trusted episode. Corvus death, requester death, timeout, malformed
semantic frames and stale END/submission messages all fail closed. The existing
workspace must not be treated as proof that authorization completed.

## Verification before declaring it usable

- Kernel tests for registration privilege, non-transferability, generation checks,
  owner death and concurrent entry/exit.
- Broker tests for forged resource IDs, arbitrary DMA/scanout commands, stale fences,
  and client resources aliasing trusted backing.
- Physical versus injected attention; held modifiers; no Enter/Escape replay;
  no episode bytes in compositor queues, screenshots or transcripts.
- Real screenshots: empty, pending, wrong key, lockout, expiry, requester gone,
  cancellation and success, at minimum and reference resolution.
- A playing audio stream, mounted Haul service and fullscreen Gallery across the
  episode; live accelerated contexts must survive. Test current v2m, ITS and shared
  INTx modes, then the repository's required SMP/UBSan verification.
- Update the architecture, trusted-path specification, operator manual and owning
  Vault dossiers. Continue single-agent self-review as instructed, explicitly not
  an independent audit.

## Alternative

Keep the strict kernel-owned-pixels model by moving GPU queue/scanout arbitration
and trusted input ownership into the kernel. This requires reopening the
no-graphics-in-kernel boundary and mediating the existing accelerated resource
protocol there; it is substantially larger than adding a simplefb blitter.
It can preserve the stronger kernel-only output claim, at the cost of a larger
kernel driver/parser surface. A second dedicated virtual GPU would avoid some
mediation but would be a QEMU-specific workaround and is not recommended.

## Implementation checkpoint

The QEMU path is implemented in Lictor, Corvus and the kernel seat endpoint.
Warden starts service and normal compositor as independent narrowed leaves.
The normal transport uses nonblocking /srv endpoints: POLLOUT alone is not a
whole-reply guarantee. Native ABI 121/122/123 covers seat operations, peer-bound
backing import and shared open-file nonblocking mode.

Graphical grants publish held under the grant lock and cannot be redeemed until
SEAT_RESTORED commits the exact target incarnation/session. Failure before that
commit cancels the grant. Corvus replies after restoration, but the kernel barrier
also protects against a requester that polls /use independently of the reply.
Serial SAK requires an explicit immutable boot token; the graphical regression
disables it. The current visual backend uses the approved neutral field.

Measured integration evidence: host framing/model/ownership/render tests; kernel
seat gates and transport backpressure tests; QEMU empty/confer/DAC/abdicate/wrong
key/cancel flow with real screenshots. Accelerated context continuity, broader
backend qualification and final matrix verification remain separate gates.

### Trusted raster memory

The private CPU raster workspace is committed at Lictor startup in its own
anonymous, non-exportable mapping, sized by the validated display geometry
(maximum 4096 by 2160 pixels). It is separate from both the general-purpose
heap and device-visible plain-DMA scanout backing. Entry and repaint reuse
it rather than allocating a framebuffer alongside normal broker bookkeeping.
Both buffers are erased after restoration. This prevents normal resource churn
from crowding out the allocation required for secure attention; inability to
reserve the workspace fails service startup before normal clients are admitted.
