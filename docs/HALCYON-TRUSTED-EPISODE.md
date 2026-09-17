# Halcyon trusted episode

Status: visual direction and Lex curiata visual specification approved by the
operator, 2026-09-17; implementation design below. The operator
requested a graphical SAK design alongside aux integration. This document
specifies the experience and the boundary required to implement it; it does
not claim that the current virtio-gpu session is a trusted framebuffer sink.
It refines TRUSTED-PATH sections 5–9 and IMPERIUM-DESIGN section 11.7.

The approved, self-contained [visual preview](halcyon-lex-curiata-preview.html)
is retained with this specification. It is a labelled design artifact, not a
running authorization surface.

## Purpose and presentation

The *lex curiata* is the conferral act. A secure-attention episode is a temporary change of display and input owner.
It is not a Halcyon window, workspace, modal dialog or Beacon object. The
operator uses the physical attention gesture, reads the authority Corvus is
about to confer, and either authenticates that request or cancels it. On
completion the previous workspace returns with the same layout and focus.

The operator selected a full-screen takeover with the workspace frozen, dimmed
and softly blurred behind a centred Halcyon-styled dialog. The entire display
belongs to the trusted sink: the panel is not a regular compositor surface.
A narrow top rail identifies CORVUS and LEX CURIATA. The panel is titled
**Conferring imperium**, its authority section **Provincia**, and its lifetime
section **Term**. These retain the documented Roman vocabulary while plain
explanations make the authorization understandable without knowing the theme.

Capture a kernel-owned, immutable copy of the last fully presented frame only
after exclusive scanout ownership is established. Apply a bounded blur once,
then dim it to keep the dialog dominant; no userspace-owned buffer remains as
the backdrop. Do not animate or refresh that snapshot during the episode.
If safe capture is unavailable, use the same dialog on a neutral dark field.
Failure to capture must never fall back to a live compositor backdrop.

The panel follows Instrument's spacing, fine rules, restrained amber and clear
field labels. Its resources are build-baked and independent of user themes,
fonts and plugin content. Use a fixed dark palette for the trusted scene,
regardless of the suspended workspace's palette, so its contrast is known.
Restore the user's theme unchanged when the episode ends.

## Reading order

At a 1280 by 800 reference display, use a 48 px top rail and a 640 px wide
reading column centred horizontally. Start its content near y=160. Keep the
bottom help line at least 40 px above the display edge. At smaller sizes,
reduce outer margins before reducing the font; wrap values and retain every
requested capability. Never ellipsize a capability, principal or scope term.
For an insufficient display, fail closed and offer the configured recovery
medium rather than hiding part of the authorization.

The rail and persistent attention indicator are drawn by the sink. Corvus
supplies these ordered fields:

1. **Confer imperium** — the action title.
2. The principal and requesting process, obtained from the kernel-stamped
   request, not a command-supplied title. A command label can appear separately
   as explicitly untrusted context; it cannot replace the identity.
3. **Provincia — authority requested** — a row per capability, with its exact capability
   name and a plain explanation. POST_SERVICE means bounded service posting;
   DAC_OVERRIDE means bypassing file permission checks; CHOWN changes file
   ownership; KILL can terminate other principals' processes. Show only the
   requested subset, not the entire eligible level.
4. **Term — lifetime** — this scope and its descendants; revoked on root exit,
   abdication or expiry. Show an actual deadline when one exists. Do not invent
   a time limit for a request with none.
5. **Imperium key** — a labelled secret field, followed by explicit action
   hints. The sign-in password and the distinct imperium key must not be
   described as interchangeable.

Keep authority text bright and the explanations quieter, without relying on
colour alone. KILL adds a textual warning beside the fasces with its axe;
non-KILL requests use the rods without the axe. The fasces is explanatory
iconography, not evidence that the display is trusted. Likewise, a screenshot
of this scene is never a security credential: the physical gesture and the
exclusive ownership transition establish the trusted path.

## Interaction and episode states

- **No pending request:** state that no authorization is waiting. No secret
  field is shown. Escape returns to the workspace. SAK itself grants nothing.
- **Request pending:** show all immutable request fields before accepting a
  key. Enter submits only from the key field; opening the scene never submits.
  Escape cancels the pending request and ends the episode. No click-through
  reaches the suspended input route.
- **Verifying:** disable a second submission, retain the request summary and
  show a static verification label. Do not spin an animation that requires
  the regular compositor. Audio and networking continue.
- **Wrong key:** clear the input storage, preserve the authority summary and
  display Corvus's failure state. Retry and lockout follow the existing policy;
  the visual design adds no parallel authentication counter.
- **Expired or requester gone:** clear the input, explain that nothing was
  granted, and let the operator return. A request identity or capability-set
  change invalidates the form instead of silently updating it under a key.
- **Granted:** say that the named scope was authorized, then end the episode
  through the trusted owner. Halcyon subsequently shows the elevated shell's
  existing fasces prompt. A grant does not create a permanent elevated desktop.
- **Corvus failure:** revoke episode input/output ownership and follow the
  kernel's existing fail-safe termination contract. A compositor must never
  decide that a stalled prompt is safe to accept or dismiss on Corvus's behalf.

Consume the entering attention gesture, held keys and their releases across
the boundary. On restoration clear stale modifiers and discard episode keys;
do not replay Enter or Escape into the application. Restore the previous
focus only if its process and surface are still live; otherwise select the
normal safe workspace focus. Running clients may have exited or resized while
the episode was active.

## Corvus, the sink and Halcyon

Corvus remains the authority broker. Its existing provincia composer is the
starting point: it creates bounded, medium-independent cells and uses the
same request and verdict states for serial and framebuffer presentation.
Split composition from serial ANSI emission before adding a framebuffer ABI.
Corvus supplies content; the sink owns framing, fonts, colours, the attention
indicator and the secret-input route. Beacon is deliberately not the protocol
between these two components: it describes untrusted application content and
cannot confer trusted status on a frame.

The initial framebuffer implementation should accept a fixed maximum cell
grid with a small closed set of semantic attributes, not pixels, font files,
paths, terminal escapes, or general layout commands. Validate dimensions,
lengths and attributes before replacing the current frame, so a malformed or
short update cannot partially obscure a previous authorization. The kernel
may rasterize a build-baked, bounded font into a kernel-owned linear buffer.
User-selected Instrument themes cannot alter this font or palette.

The kernel's episode generation binds all submissions and END operations to
the current trusted owner and current episode. A stale owner, a delegated
handle or a delayed frame from an earlier episode cannot draw, read secrets
or end a later episode. Secret bytes belong only to Corvus and its exclusive
input path; they never enter the Halcyon transcript, PTY, clipboard, snapshots,
logs or persisted session model. Clear them on every completion and error path.

Halcyon continues maintaining the workspace while its scanout and input route
are unavailable. It receives only suspension/restoration state, with no key or
authorization payload. A restored compositor repaints the entire scanout; it
must not reuse an assumed-valid dirty rectangle from before the takeover.
Nocturne continues its real-time cycle throughout, as I-46 requires.

## Current hardware boundary

TRUSTED-PATH section 7 explicitly restricts a kernel-rendered episode to a
kernel-reachable linear framebuffer. The present QEMU virtio-gpu path is owned
by userspace. Painting a Corvus-themed Halcyon surface over it would not meet
the invariant, even if input focus were moved and every other window hidden.
That environment retains the serial BREAK trusted path until an approved
trusted scanout mechanism exists. Any mirrored serial scene is labelled as a
mirror, and never accepts a key through Halcyon's renderer feed.

A graphical implementation therefore needs either the documented linear
framebuffer backend and trusted keyboard, or a separately designed trusted
virtio-gpu handoff. The latter would change the display trust architecture and
requires an explicit design review; it is not smuggled into this visual work.

## Implementation and acceptance

1. Extract and test the bounded Corvus presentation model without changing
   authorization policy or serial output semantics.
2. Implement episode-bound cell submission and the linear-framebuffer sink,
   with exclusive mappings and trusted input, on supported hardware.
3. Wire Halcyon suspension and complete redraw on restoration, retaining the
   serial fallback for virtio-only images.
4. Verify request, empty, wrong-key, lockout, cancel, expiry, success and trusted
   owner death. Capture each visual state on both the smallest supported
   framebuffer and the reference display.
5. Attempt ordinary-client painting, input injection, stale submissions,
   simultaneous requests and delayed END calls during the episode. Assert no
   authority grant without the physical attention transition and valid key.
6. Exercise a running audio stream, a remote mount and a fullscreen Gallery
   surface across entry and restoration. Assert audio progress, intact remote
   operations, no replayed keys and correct focus after the viewer exits.

The screenshots for this design are visual specifications. They must carry a
visible DESIGN PREVIEW label until the trusted sink is implemented and the
ownership tests pass. No demonstration should train an operator to type the
imperium key into an ordinary Halcyon pane.
