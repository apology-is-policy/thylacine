# Lex curiata backdrop: completed private pixels

Status: closed without implementation, September 24. The operator accepted the
current solid background: "We don't have to blur the background. The current
solid is sufficient." The neutral private field is the selected presentation,
not an outstanding visual defect. No presentation-copy mechanism is authorized
or required by this decision. The proposal below is retained for architectural
reference only; its recommendations and options are historical.

## Finding in the current tree

Lictor owns the GPU transport, admits the normal compositor through its broker,
and parks normal requests during a trusted episode. `Device::presentation`
retains a resource ID and its geometry, sufficient to restore that presentation.
`Pin` retains imported DMA backing but does not make its contents immutable.
The normal compositor can still write those pages while admission is parked.
Copying them after SAK would therefore capture an arbitrary in-progress buffer,
not necessarily the completed frame shown to the operator.

The plain 2D path exposes upload and flush, with no implemented private readback
operation. The existing 3D readback writes into the resource's attached backing;
using ordinary backing for that readback would still leave a client-writable
alias. `Seat::step` currently passes `None` to every trusted paint.

## Precedent and fit

Plan 9's [draw device](https://9p.io/magic/man2html/3/draw) mediates drawing and
image readback through the image owner. That supports an owner-mediated image
operation rather than a privileged observer scraping an application's memory.

Genode's [GUI stack](https://genode.org/documentation/components) separates GUI,
capture and event sessions; framebuffer drivers consume capture output. Its
[session model](https://genode.org/documentation/genode-foundations/20.05/components/Common_session_interfaces.html)
also charges client framebuffer storage to an explicit quota. The relevant
pattern is an explicit output boundary with bounded storage, not a screenshot
request to an arbitrary application.

Fuchsia's [Screen Capture](https://fuchsia.dev/fuchsia-src/concepts/ui/scenic/screen_capture)
uses registered output buffers. It is useful precedent for a separate capture
destination and completion contract; importing its public screenshot interface
would be inappropriate for Thylacine's private authorization pixels.

These are architectural comparisons, not evidence that those systems implement
Thylacine's SAK threat model. The existing Lictor owner and generation boundary
remain authoritative here; no new authority should be delegated to Halcyon.

## Options

### A. Owner-mediated presentation copy (recommended)

Make a completed normal presentation available to the trusted owner as private,
bounded pixels. A software/linear backend maintains a broker-owned shadow of the
normal scanout upload; a GPU backend may implement private readback instead.
Both discharge the same capture contract. The plain QEMU 2D implementation
would need the shadow route; Pi backends must independently qualify theirs.

For the 2D route:

1. Track the current presentation's actual image class, format, dimensions,
   row pitch, backing range and incarnation. Opt in only for a fully understood
   linear 32-bit image. Unsupported formats/paths retain the neutral fallback.
2. Copy the relevant client source rectangle into private ordinary-presentation
   backing before the GPU reads it. Attach that private backing to the normal
   resource, so the bytes retained by the owner are the same bytes uploaded.
   No private authorization pixels ever enter this resource. Preserve every
   normal upload's offset, stride, completion and error semantics.
3. Maintain full-frame coverage and invalidate the capture candidate on any
   unsupported mutation, context attachment, resize, unref or ambiguous device
   result. Restore the ordinary backing before returning an operation to its
   old path. A reused resource ID must not inherit an old candidate.
4. At takeover, park admission and retire all normal work. If necessary,
   complete the final normal flush before replacing scanout. Establish exclusive
   trusted output, then copy the owner's completed ordinary pixels into the
   episode-private snapshot. No untrusted writable alias remains.
5. Apply a bounded blur once and dim to the preview's 26 percent brightness.
   Reuse this immutable snapshot for all semantic/mask updates. Erase it after
   restoration and on every abandoned candidate/error path.

Bounds: retain at most one eligible normal presentation, at the existing maximum
4096x2160. A 32-bit image is 35,389,440 bytes; shadow plus snapshot add at most
70,778,880 bytes, not one allocation per normal GPU object. At 1280x800 the
pair costs 8,192,000 bytes. Precommit storage before admission, and account for
DMA allowance separately from CPU-only storage. Allocation failure disables
capture, not authorization. Full-frame copies at 60 Hz would add about
234 MiB/s at 1280x800, so preserve dirty-rectangle uploads and measure the actual
steady-state cost; the snapshot/blur itself runs only once per episode.

This changes the normal presentation implementation and its memory budget. It
deserves a separate reviewed implementation rather than being hidden in a
cosmetic renderer patch. The proposed backend contract is portable; the QEMU
adapter alone does not qualify Pi hardware.

### B. Retain neutral output until each backend has private readback

Keep today's normal presentation path untouched, finish the dialog fidelity,
and implement backdrop capture only on backends with a demonstrated protected
readback operation. This avoids an extra copy in ordinary rendering, but the
plain 2D QEMU path remains visually different from the approved preview.

### Rejected: copy the live compositor buffer at SAK

Parking broker requests does not revoke CPU writes through existing mappings.
Such a copy could be torn, stale or deliberately changing. It would look like
the requested feature while silently weakening the completed-frame contract.
Neither a Halcyon-supplied screenshot nor a QMP host screenshot belongs in the
production trusted path.

## Required evidence for A

- Pure range/stride/coverage tests, including partial uploads, malformed ranges,
  resource reuse, unsupported formats and memory-budget exhaustion.
- A writer mutating its old buffer after upload cannot alter the captured
  completed pixels. Delayed/failed GPU completion produces neutral output.
- Trusted pixels and key bytes never reach a normal resource, mapping, import,
  broker reply or capture export. Test failure and cleanup order, not only the
  successful screenshot.
- Real QEMU pictures for request, masked input, verdict and restored workspace,
  with F10 grant/deny/cancel/expiry/lockout/recovery still green.
- Measure normal frame cost and takeover latency on the qualifying backend.
  Record Pi 400/500 qualification as unperformed until run on those boards.
