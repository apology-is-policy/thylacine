# Aux integration with Halcyon

Status: integration verification complete, 2026-09-17. Integration checkout:
`codex/aux-halcyon`, incorporating committed `aux-3` through `b0ea1986`.
The operator requested single-agent work. This record is self-review and
measured verification, not an independent adversarial audit.

## Resulting behavior

View images occupy their command's position in the live and frozen Halcyon
transcript. Bounded per-tile caches share the session allowance; a stale pane
or a changed admission budget cannot receive a successful placement reply.
Gallery composites transparency over black, preserves image aspect ratio,
names its native pane, zooms with the workspace controls, and restores the
shell on Escape or Q. The manual reader emits Beacon headings, code and tables
in Halcyon and readable text on serial or through a pipe. Six checked sections
cover the reader, remote files, View, Gallery, Nocturne and DOSBox-X.

Nocturne, the SDL port and DOSBox-X share the current compositor and capability
model. SDL forwards application titles and restores title and dynamic-frame
intent after recreating a surface for a video-mode change.

## Architectural findings and dispositions

* Inline placement previously preceded the command that requested it. A
  correlated Beacon object anchors the raster in transcript order; the image
  remains out of the text stream and subject to explicit cache bounds.
* Gallery previously discarded alpha. Straight-alpha pixels now composite
  over its black canvas; tests distinguish transparent and translucent cases.
* Device ordering previously avoided shared PCI interrupt lines by assumption.
  The approved PCI design replaces that assumption with function-owned shared
  INTx endpoints and protected MSI-X routing, including GICv2m and ITS/LPI.
* Last TCP clunk discarded queued writes. The approved retirement design gives
  transports a separate bounded owner through graceful close or deadline.
  Public slot reuse and Weft detach remain immediate.
* The pointer-only `Dev.open` interface loses typed errors. The current repair
  uses the existing per-unpublished-Spoor error pattern, read before clunk,
  to preserve 9P resource errors. A typed result throughout the driver vtable
  remains an architectural improvement; no global or thread-local errno was
  added as a substitute.
* The approved Lex curiata scene is a visual/trust design. Ordinary userspace
  GPU rendering cannot serve as the trusted authorization display. Corvus
  serial SAK remains implemented; the graphical trusted sink is not implemented.

## Self-review

The PCI review traces capability ownership from the function claim through
endpoint publication, dispatch pins, ticket completion and last close. Domain,
configuration and controller locks have explicit ordering. Controller command
completion waits are bounded and occur outside IRQ/domain locks. A failed
hardware drain quarantines the vector; it does not return uncertain storage or
routing to the allocator. Fault fanout wakes endpoint waiters outside controller
locks. Protected table/PBA pages cannot be mapped by an ordinary driver claim.

The TCP review traces the last public reference, socket transfer, slot reuse,
Weft detach, admission counts and timeout reaping on the single event loop.
Retirement storage is reserved before close. Accepted-listener replacement
checks the same total transport bound. Unread receive bytes cannot pin FIN;
the polling deadline includes retirement expiry. Resource-admission retries
are limited to ENOMEM, bounded, diagnosed, and included in benchmark timing.

The graphics review checks raster lifetime, tile identity, byte allowance,
text/raster ordering, pane ownership for titles, foreign-pane counts, and
cleanup on viewer exit. SDL metadata belongs to each compositor surface and
must be restored when that surface is recreated. The manual's strict parser
rejects unsupported input before emitting its Beacon representation.

## Measured verification

* Kernel: 1,570 tests on GICv2m/HVF, ITS/TCG and shared INTx/TCG. Controls include
  actual MSI-X DMA/PBA delivery, ticket replay, table readback failures,
  allocator quarantine, ITS command-ring saturation/deadlines, shared-line
  recovery, and 64 rounds of last-close/dispatch overlap.
* Networking: each backend receives all 8,388,608 expected bytes at the host
  after immediate client close, plus 200 immediate and delayed round trips
  and 40 dials. Deterministic guest controls cover unread RX, slot reuse,
  Weft detach, TIME-WAIT, capacity refusal/recovery and deadline expiry.
* Audio: twelve cases cover byte-stream mixing, ring playback, SDL playback
  and capture permissions across the three interrupt backends. Captured audio
  contains both simultaneous tones and the silent tail.
* Applications after TCP repair: manual, real-Pi Haul mount and post, Haul
  hangup, DOSBox display, input/foreground return, configuration and dynarec
  all pass. The Halcyon media scenario also passes after the repair.
* Host logic: Halcyon 325, Tapestry 104, Gallery 11, View 11 and manual 72 tests
  pass; all six manual sections pass the checker. The DTB parser passes 29
  native fixtures. Thirty-six relevant existing negative model configurations
  produce the expected counterexamples (24 PCI-related and 12 network/9P).

The final DOSBox session gate passes in 71 seconds after the SDL metadata
repair; tiled, zoomed and returned-to-shell captures were visually reviewed.
The full default/UBSan by SMP4/SMP8 gate passes all forty boots (ten per
configuration), with zero corruption, external kills, missed injections,
timing classifications or other failures. Test exposure totals 2,313 seconds.
An additional eight-CPU ITS/TCG UBSan boot passes in 121 seconds, with all
1,570 kernel tests and all three resident drivers using MSI-X.

Resident drivers were measured under automatic selection on all three
controller configurations and colliding PCI layouts. The `PCI_IRQ_MODE`
environment override was not separately exercised in a resident-driver boot;
explicit mode claims are covered by kernel tests. QEMU evidence is not a claim
of physical-hardware validation or reliable delivery after network failure.
