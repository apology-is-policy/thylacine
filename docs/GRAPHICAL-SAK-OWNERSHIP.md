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
  user themes, images, font files, configuration or arbitrary UI programs. The one
  thing it reads is the platform's display scale, at its start ("The trusted
  screen's scale" below), which chooses between two baked renderings.

## Entry and restoration

1. A physical reserved attention gesture (Ctrl-Alt-Delete, or Ctrl-Alt-F10 where
   the keyboard has no Delete) reaches the trusted input owner and is scanned by
   the kernel from that owner's key reports. Software
   input injection is a distinct untrusted route and cannot trigger SAK or enter
   the secret queue. Serial BREAK remains a recovery trigger only in a configured dev/recovery posture.
2. The kernel starts a generation and freezes normal presentation/input admission.
   The trusted owner drains previously admitted presentation fences before acknowledging
   exclusive trusted scanout. Corvus cannot accept secret input before this acknowledgement.
   The October 2 approved clipboard extension additionally requires Tapestry's
   exact-generation aggregate cancellation acknowledgement before this ACK.
   A stalled participant uses the existing five-second episode timeout, with
   no grant or fallback. See `HALCYON-INTERACTION-SEAT-REVIEW.md` option A.
   That userspace barrier is not yet implemented; display exclusion alone is
   not proof of clipboard cancellation.
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

Two different things fail, and they are treated differently.

An EPISODE fails (a deadline passes, a device step is refused, a frame does not
decode, Corvus or the requester goes away). The kernel cancels the held grant,
scrubs the secret queue and closes the episode it opened. The owner then shows
a failure notice, waits for every key to come up, puts the last normal
presentation back and reports RESTORED; the kernel returns the seat to normal
and releases nothing. Fail-closed is about AUTHORITY: nothing in these rules
requires the display to stay dark, and before 2026-09-21 it did -- a chord held
past the 5 s quiesce deadline, or a prompt left idle past the exclusive
deadline, left the machine without a display until both seat processes died.
A seat failure closes only the episode the seat opened; a serial episode, which
opens only while the seat is normal, is never closed by one.

The OWNER fails (it dies or stalls). A dead/stalled trusted service never causes
the compositor to inherit display or input ownership. Cancel the pending grant
and invalidate the generation. Serial
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

### Takeover review, 2026-09-21

The work above was committed as a checkpoint (db88cf71) and reviewed by a second
agent (memory/audit_lictor_closed_list.md). Fixed: a seat failure abandoned ANY
open console episode, so an unrelated compositor death could unfreeze the
console under a serial key entry (P1); a failed seat was terminal (P2); the
broker kept naming a presented resource the compositor had unref'd, so the next
restore failed the seat (P2); the ledger reserved ids before validating the
request; and the last red gate was a probe that sampled the terminal foreground
once, racing the shell's own post-spawn handoff. Added: Ctrl-Alt-F10 as a second
reserved chord, `cons.graphical_seat_grant_and_failure`, and
`ls-graphical-sak-recover`. Open items are listed in the Lictor dossier.

### Trusted raster memory

The private CPU raster workspace is committed at Lictor startup in its own
anonymous, non-exportable mapping, sized by the validated display geometry
(maximum 4096 by 2160 pixels). It is separate from both the general-purpose
heap and device-visible plain-DMA scanout backing. Entry and repaint reuse
it rather than allocating a framebuffer alongside normal broker bookkeeping.
Both buffers are erased after restoration. This prevents normal resource churn
from crowding out the allocation required for secure attention; inability to
reserve the workspace fails service startup before normal clients are admitted.

### The trusted screen's scale

The trusted screen is one layout, in layout pixels, drawn at a whole number of
device pixels to a layout pixel: one or two. Its scale is the nearest whole one
to its display's, two from 150 percent, taken only if what is left of the
screen in layout pixels is still the 800 by 720 the layout needs; else it is
one. The display's scale is the one the session derives, by the session's own
rule (`docs/HALCYON-SCALE.md` section 3): the platform's `thylacine.scale` on
the kernel's command line first, then the display's millimetres from its EDID,
else 100 percent; and it is carried to the screen's own pixels when the screen
is not made at its display's size. Lictor reads both sources itself, at its
start. The size and the scale are chosen once, there, by one pure function.

The type is baked at each scale. The set at two has the box, the baseline, the
bearing room and the advance of the set at one, times two, and only the outline
is drawn finer, so every word is on the same line and in the same place at both
scales and the layout is verified once. Nothing is scaled at run time: a rule,
a border and a field are whole layout pixels, and a glyph is a baked mask. The
rule above about fixed baked fonts stands. The rule beside it, that the service
loads no configuration, has this one exception, and it is the platform's word
and no user's: the kernel's command line and the display's EDID can choose
which of the two baked renderings is drawn and nothing else, and where neither
says, or what they say cannot be drawn, it is one.

Three decisions were made for this by the implementing agent under the
operator's standing authorization of 2026-09-01 (a recommended design in line
with the heritage is accepted while the operator is away), and are the
operator's to change:

1. **Whole scales, with the type baked at each.** A fractional scale needs
either a rasteriser on the trusted path or masks resampled at run time, and the
first is what the baked type exists to keep out. Plan 9's fonts are the
precedent: a font is a set of bitmap subfonts made for one size, and a larger
display is given a font made larger, not one stretched.

2. **The scale's source is the platform, never a client.** The declaration and
the EDID are the two things the session's own derivation starts from, and
Lictor reads them itself. The compositor's scale, a session's setting and a
surface's scale are not asked: they are a client's word, and the trusted screen
must look the same whatever the session it suspended has been told.

3. **Two from 150 percent.** At 150 the screen at two is a third larger than
the session's type and at one a third smaller; larger is the readable side for
a screen that asks for a key. From a display's millimetres the session's rule
gives the percent in quarters, rounded half up, so on that path two begins at a
true 137.5 percent, 132 pixels to the inch. A display too small for the layout
at two is drawn at one whatever its scale, which is every panel of 1920 by
1080 or 1200 at 150 percent: 540 and 600 layout rows are under the 720 the
layout needs.
