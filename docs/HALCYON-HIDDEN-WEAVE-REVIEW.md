# Hidden terminal graphics: scope and lifecycle review

October 4, Astra. Option 1 APPROVED by the operator: "I will go with your recommendation."
HI1-R30 blocks claiming the larger live-tile clipboard pressure qualification.

## Observed behaviour

At 1280x800, a workspace with eleven parked foreground controllers and twelve
terminal-host bindings cannot add another full-size tab. The actual map charge
is 30387 pages plus a new 2613-page triple-buffer weave, above the existing
32768-page (128 MiB) per-address-space shared-mapping limit. Hidden tabs stop
receiving FRAME ticks but retain their full mapped pixel buffers. Terminal
transcript/model state is separate from those pixels.

The map refusal unwinds the new surface correctly: libtapestry's fail_created
sends destroy; Tapestry retire closes the hosting leaf. Halcyon's subsequent
close gets ENOENT. Existing tabs survive, but the user sees no clear resource
explanation. This is not clipboard credit exhaustion. Raising service-ring
credits cannot repair it. A half-width run independently exposed HI1-R31's
PTY fid undercount; correcting six handles per ordinary shell preserves the
existing sixteen-pair bound and does not solve pixel retention.

Evidence: work/oct4-hi-pressure/graphics-1791093104443341000, including UART,
source manifest and screenshot. Original full-size pressure remains FAILED.

## Options

1. **Add cooperative hidden-buffer suspension now (recommended).** Preserve
   the surface identity, placement, terminal dimensions, running job and parsed
   transcript while releasing hidden pixel mappings and backing. Recreate pixels
   and require a complete repaint before showing the tab again. Keep current
   memory ceilings. This expands the clipboard arc into a Tapestry/libtapestry/
   Halcyon graphics lifecycle change, with dedicated stale-generation, pending
   present, visibility-race and SAK tests. It does not increase the sixteen-PTY
   limit or promise thirty-two terminal windows.
2. **Keep the present graphics lifecycle for this delivery.** Make capacity
   refusal explicit in Halcyon, finish clipboard/modal work on admitted tiles,
   and qualify the clipboard protocol reserve separately. Retain the failed
   large-workspace gate and schedule hidden-buffer work as its own arc. Lowest
   immediate change risk, but the resolution-dependent tab limit remains.
3. **Raise the mapping ceiling.** Small implementation, larger retained memory
   and later failure at another resource limit; poor fit for Pi targets. Not
   recommended. This would itself change a kernel resource-policy contract.

## Proposed boundaries for option 1

This is cooperative graphics storage management, not stopping application
processes. Visibility is compositor-authored; clients must never infer it from
absence of FRAME ticks or from focus (a visible unfocused tile is not hidden).
Opt-in clients receive a versioned lifecycle notification and acknowledge after
stopping pixel writes and retiring submitted presents. Unsupported clients keep
the existing behaviour and pay their existing quota. A slow or refusing client
cannot block rendering, input or SAK; failed admission remains bounded and visible.

The client closes the old weave mapping through its owning fid. The compositor
retires backing only after actual backend completion/release, with stale share
IDs and generation-tagged presents invalidated. CPU composition, scanout, GPU
imports and asynchronous backend references require the same explicit ownership
proof; a QEMU observation is not a release fence. The surface record and semantic
geometry remain live. Do not implement suspension by destroying Surface, faking
a 1x1 resize, or adding a second terminal model.

On reveal, allocate a fresh weave generation within the unchanged ceiling, map
through a fresh fid, repaint completely and publish only after the first complete
frame. Keep focus/input targeting tied to the same live pane identity; do not
show uninitialized pixels or accept stale-generation output. On allocation
failure preserve the hidden job/model and report refusal. Define and test rapid
hide/show reversal, resize while hidden, termination, logout and SAK before
activation. This will need an explicit protocol/state specification before code;
no new kernel authority or syscall is proposed.

## Prior art and fit

Plan 9 draw separates server-backed refresh from client repaint notification
(Refbackup versus Refmesg). Its manual also says Refmesg is not fully implemented;
this is a useful conceptual precedent, not proof of a drop-in implementation.
[Plan 9 draw](https://9p.io/magic/man2html/3/draw).

Genode's GUI sessions pay for virtual framebuffer storage from session quota.
The useful lesson is explicit bounded accounting, not an unlimited hidden cache.
[Genode common session interfaces](https://www.genode.org/documentation/genode-foundations/25.05/components/Common_session_interfaces.html).

Fuchsia Flatland image lifetime follows retained references; buffer retirement
must wait for compositor/display use to end. This supports an explicit release
protocol rather than equating invisible with safe to free.
[Flatland image lifetime](https://fuchsia.dev/fuchsia-src/concepts/ui/scenic/life_of_a_flatland_image).

Thylacine already has generation-based reweave, fresh weave fids, completed
present handling and semantic terminal state. It lacks a hidden-storage lifecycle:
TEV_FOCUS is not visibility, zero-sized CONFIGURE is ignored, and Surface::drop
retires the surface identity. The proposed combination reuses those mechanisms
without inventing a second authority owner. No novel security claim is made.

## Decision (October 4)

The operator selected cooperative hidden-buffer suspension for this delivery.
The alternatives of retaining the present lifecycle or raising the mapping
ceiling were not selected. The concrete contract is in
[TAPESTRY-STORAGE](TAPESTRY-STORAGE.md); commit that scripture before code.
This is scope approval, not a claim of implemented or qualified suspension.
