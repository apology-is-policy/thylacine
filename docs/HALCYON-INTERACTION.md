# Halcyon interaction: pointer, modal text and session clipboard

Status: APPROVED for implementation, 2026-09-24. The operator reviewed this
specification and said, "I love it. Let's start." The hand-built 9P service and
later Mycelium migration remain explicit choices. Implementation follows HI-0
through HI-5; approval is not a runtime verification claim. This document does
not reopen the separate user-authority arc.

Companions: HALCYON.md, HALCYON-INSTRUMENT.md, HALCYON-SCALE.md,
HALCYON-TYPE.md, BEACON.md, UT-NORA-ERGONOMICS.md, BROWSER-DESIGN.md,
GRAPHICAL-SAK-OWNERSHIP.md and GRAPHICAL-SAK-PORTABILITY.md.

## 1. Outcome and scope

The pointer is visible before browser testing. Text interaction feels familiar
across shell transcripts, Nora and native browser fields. One status widget
reports the focused interaction's actual mode. Text can be copied between
applications without copying Beacon formatting or executing pasted commands.

Deliver pointer rendering; a per-session UTF-8 clipboard; a shared modal action
vocabulary; transcript character/word navigation, search and selection; Nora
clipboard/mode integration; and a reusable native text-field integration for
Boosty. Boosty's web content remains owned by its browser track.

Not included: Mycelium, a new kernel IPC mechanism, clipboard history or disk
persistence, cross-user clipboard sharing, host/guest clipboard synchronization,
image/file clipboard formats, general drag-and-drop, or FFmpeg/video playback.
These are separate extensions, not silent promises of this tranche.

## 2. Starting point and architectural choices

Inspected Astra 743d3084 and Main's working checkout 7c54ef71. Reconcile current
Main/Aux before implementation; these are source observations, not fresh tests.

- halcyond input.rs and session.rs have INS/NOR transcript modes, row selection,
  local yank/paste, and object navigation. w/b currently move between objects;
  bare d/u also scroll. These collide with Nora's text-editing vocabulary.
- Nora editor.rs owns NOR/INS/VIS, command entry and Space menus. Its register is
  local. Its host performs I/O; the pure editor raises requests.
- Session paneplace.rs serves per-pane inline media over one
  /srv/halcyon-<user> listener, using a pane route token and kernel peer principal.
  It currently permits two concurrent connections: that limit is not adequate
  for a persistent connection from every application's new control client.
- Tapestry tracks and routes pointer motion/buttons and divider capture. No
  complete portable native pointer-image path was found in this inspection.
- The Instrument status bar is an existing chrome surface. Extend its mode
  widget; do not introduce a second bar or a competing status layout.
- Mycelium remains planned. Use SrvConn + existing ninep codec and readiness
  mechanisms. The new service protocol is deliberately small and versioned.

Ownership:

| Component | Owns |
|---|---|
| tapestryd | Pointer position, hit testing, capture, cursor composition, authoritative focus/surface lifetime |
| halcyond session | Transcript interaction, active-mode presentation, clipboard storage and session interaction broker |
| Nora / native application | Its own document/field state, selection, local registers, undo, and mode transitions |
| kaua-term + foreground job integration | Which terminal application owns input and the explicit native-app reporting bridge |
| shared userspace interaction library | Actions, key sequences, selection/clipboard types and protocol codec; no process-global editing state |

Do not move editor state into the compositor. Do not load Nora's editor engine
into Halcyon. Share pure rules where semantics really coincide; supply adapters
for a mutable document, immutable transcript and single-line field. Avoid a new
universal widget toolkit as a prerequisite.

## 3. Exactly one keyboard owner

Each focused text context has a controller, context ID and monotonically
increasing context epoch. A controller belongs to an authenticated live process
incarnation and a particular live surface or terminal foreground owner. A pane
route token locates a pane; it does not prove ownership of its active application.

A terminal starts with Halcyon transcript control. When a participating native
foreground application takes the terminal, it owns Escape and its mode. When it
exits or loses foreground ownership, that registration expires. An unknown
application displays APP, not a guessed INS/NOR. Alternate-screen status alone
is insufficient proof of controller identity. Background processes, stale PIDs,
and arbitrary escape sequences in command output cannot register a controller.

Direct graphical clients bind through their existing Tapestry surface ownership.
Terminal clients bind through the terminal host's authenticated foreground-job
record. This record must be an explicit integration with the actual PTY/job
owner; do not infer it from an executable name, tag text or the latest writer.
Before a foreground handover completes, its owner revokes the previous controller
and publishes the new owner epoch through the broker/Tapestry control path. Input
and clipboard admission for the new epoch start only after acknowledgement; old
pending requests are cancelled. Application death invalidates the registration
without depending on cooperative unbinding. This ordering is part of the existing
job-control integration, not a second independently sampled foreground table.
Children do not inherit a controller registration. Libraries re-register after
exec only through the same owner checks. A job with several foreground processes
needs the host to nominate one controller; conflicting requests are refused.

Escape is delivered once to the current owner:

- INS -> NOR in a participating text context.
- VIS -> NOR, collapsing the selection by Nora's rule.
- In a menu, completion or search prompt, dismiss that overlay first and restore
  its underlying mode. A further Escape acts on that mode.
- NOR remains NOR. It does not fall through into another hidden mode machine.
- Foreign applications and games retain their Escape semantics.

A separate configurable environment action, Inspect transcript (default
Ctrl+Shift+Escape), enters read-only terminal inspection even while a foreign
application owns input. Its chord must pass the existing binding collision check.
INS/NOR/VIS then describe that explicit inspection context. Escape leaves
inspection and restores application control; it never injects Escape into the
application. No global ordinary-Escape interception is added.

## 4. Modes and the status widget

Modes on the wire: INS, NOR, VIS, CMD, APP. Read-only is a separate flag, not a
sixth editing mode. Show NOR with a read-only indication for viewers; do not
conflate readonly with the existing Nora VIEW label during migration.

The status bar always reserves the mode field while visible. Its current
Instrument typography, spacing, scale and palette roles apply. Use text as well
as color. Optional short context: NOR / Transcript, INS / Address, VIS / Nora.
Menus preserve the base mode and may show a short prefix hint. Search uses CMD.
Context labels are bounded plain text, never Beacon markup or trusted identity.

Mode updates carry controller generation, context epoch and sequence. Mode reports describe application behavior; they do not grant clipboard or seat
authority. The broker
keeps the latest update per live controller, rejects stale updates, and derives
the displayed record from authoritative keyboard focus. Pointer hover never
changes the mode widget. Focus switching preserves each controller's state.
Death, unregistration and foreground handover invalidate old reports immediately;
APP or the restored transcript state replaces them. Out-of-order IPC cannot
make a previously focused application's mode overwrite the current one.

Fullscreen does not change mode ownership. If the existing fullscreen policy
hides all chrome, it also hides this widget; do not add an overlay to games just
to display APP. Restore the correct mode with chrome. Trusted SAK uses its own
scene and does not display application-supplied context.

## 5. Text actions and clipboard policy

Default policy: Helix-style separation, accepted in the design discussion.
Local y/p operates on a context's local register; Space y/p explicitly accesses
the shared clipboard. This applies to Nora, transcript inspection and native
browser fields. Copying locally never silently overwrites the shared clipboard.
There is no per-application clipboard-first exception in this tranche.

| Input in NOR/VIS | Action |
|---|---|
| h/j/k/l, arrows | Text motion, extending selection in VIS |
| w/b/e and Nora's existing long-word variants | Word motion, not object motion |
| 0/$, Home/End | Logical line start/end |
| gg / G, Nora's existing goto forms | Buffer navigation; counts follow Nora |
| Ctrl+u / Ctrl+d, PageUp/PageDown | View movement without editing text |
| v | Enter VIS / return to NOR using the shared selection rule |
| /, n, N | Search entry, next and previous result |
| y / p | Yank selection locally / paste local register |
| Space y / Space p | Copy selection to session clipboard / paste clipboard |
| i | Enter writable context; from transcript inspection return to live input |
| ]o / [o | Next / previous Beacon object (new explicit object motions) |
| Enter on a Beacon object | Open its existing verb menu |

Nora's editing operations remain application-owned. Bare d/u stop being
transcript page-scroll aliases: they must not acquire an unrelated meaning
where Nora means delete/undo. Editing requests against old output are refused
with a short read-only indication. Unknown/inapplicable actions do not leak as
keystrokes to an underlying shell.

Nora is the initial behavioral reference, not an assumption that it already
implements every row. Shared tests must cover each supported action; add any
missing promised action (such as reverse search) before claiming parity. Keep
multiple carets and editor-specific operations in Nora; a transcript starts with
one contiguous selection. Shared clipboard export of Nora's multiple selections
joins them with LF in document order; local structured registers remain intact.

Mouse drag over selectable native text focuses the context and enters VIS.
Release ends dragging, not the selection. y copies locally; Space y copies to
the shared clipboard. A click without a drag positions the caret and collapses
the selection. Double-click selects a word; triple-click selects a logical line.
Selection and keyboard navigation use the same text coordinates and hit testing.
A mouse-created selection is not automatically a second/primary clipboard.
Web canvas interactions and foreign application mouse reporting remain owned by
the application; environment inspection requires the explicit inspection action.

In a transcript, y without an explicit selection copies the current logical
line, matching the existing row-yank use case without copying soft-wrap breaks.
In Nora, existing no-selection yank behavior remains the reference. The help
menu names the applicable selection action rather than pretending that every
read-only and mutable context has identical editing semantics.

## 6. Transcript text, search and paste

Selections use stable block/line identities plus logical text offsets, never
viewport row indices. UTF-8 slicing must not split code points; caret movement
and hit testing must not split displayed grapheme clusters. Shared word rules
must be tested with punctuation, non-ASCII text, combining marks and wide glyphs.
Horizontal motions are logical-text motions; vertical motions follow displayed
rows, retaining preferred visual x. Home/End remain logical-line operations.
Reflow changes geometry, not selected text.

Starting NOR inspection captures the current live grid into an immutable text
snapshot and anchors the view. New output continues being consumed; it neither
steals the view nor mutates the selection. Existing frozen transcript blocks can
be referenced by stable ID. Inspection uses the existing transcript budget,
not a second unbounded history. Pinned history counts toward that budget. If
retention would exceed it, cancel the oldest inspection snapshot, explain
'History expired', and clear invalid selection/search references. Never stop
draining the producer to preserve a selection indefinitely.

Search covers retained text and the captured live snapshot, not hidden Beacon
attributes. v1 search is literal UTF-8 text, case-sensitive, with forward/backward
navigation and an indicated wrap. Esc cancels preview and restores the original
position; Enter accepts it. Search runs in bounded slices so large histories do
not stall input. A 4096-byte query limit is reported, never silently truncated.

Copy exports text/plain;charset=utf-8. Preserve actual line breaks, tabs and
visible text. Do not export Beacon markup, SGR/OSC sequences, synthetic prompt
or status decorations, or layout-only soft wraps. Copy an object's visible label;
its existing verb menu may separately expose Copy path / Copy URL. Inline media
copies its textual caption/alternate text; a raster has no implicit text payload.

Paste is a typed operation, never replayed keys. The receiver reserves capacity
for the complete operation before mutation; refusal leaves its buffer intact.
Delivery acknowledgement means insertion, not merely queueing a byte prefix.
Into Nora it forms one undo
transaction; into a native field it follows that field's insertion rule. A
single-line address field rejects multiline content with a visible explanation;
it does not navigate automatically or silently rewrite line breaks. Into ut it
inserts into the editable command buffer and does not submit, even with LF.
Pasting from transcript NOR returns to live input without modifying history.
Read-only inspection of a foreground application has no implicit writable target.

Legacy PTY delivery uses bracketed paste only when the application advertises
support. Otherwise present the incompatibility and require an explicit raw-paste
action; do not claim raw bytes cannot execute commands. No unprompted fallback.
Clipboard text is not fed back through the Beacon or terminal-output parser.

## 7. Session service and application API

Extend the existing per-session pane service, retaining its current service
name and /<pane-token>/place behavior. Add /<pane-token>/interaction as a
bidirectional transaction file reached by a DIRECT connection to the existing
/srv listener, using the same ninep codec. Do not grant clipboard access by
mounting one renderer-owned connection for all applications: peer identity would
then name the mounting process rather than the requesting application.

The HALCYON_INTERACTION environment value is a locator, not authority. A pane
route token and kernel principal check remain necessary, but the controller
binding in section 3 is additionally mandatory. Tokens and clipboard contents
must not be logged. Sessions with a future duplicate-login naming scheme use
that scheme; this feature must not cement usernames as session identity.

Provide a small client adapter in the shared interaction library and a stable
C-facing API for the Pouch-based browser. Nora's pure engine raises Copy/Paste
requests; its host resolves them asynchronously. Applications do not implement
9P themselves. Mode reporting and clipboard I/O cannot block the UI event loop. Without a
Halcyon session endpoint (serial/Aurora), Nora retains local registers; shared
clipboard actions report unavailable. No host-shell helper fallback is implied.

Operations (symbolic until the HI-1 ABI commit pins numeric IDs and layouts):

| Operation | Request / result |
|---|---|
| Hello | Supported protocol version -> limits and session generation |
| BindController | Host-approved live surface/terminal-owner binding -> controller generation |
| ReportMode | Controller/context epochs, sequence, mode, readonly, label -> acknowledgement |
| GetClipboard | Foreground admission -> immutable generation, length and read-transfer ID |
| ReadClipboard | Transfer ID, offset, bounded count -> bytes |
| BeginCopy | Declared UTF-8 byte length -> staging-transfer ID |
| WriteCopy | Transfer ID, contiguous offset, bytes -> accepted count |
| CommitCopy | Transfer ID, expected clipboard generation -> new generation |
| Cancel | Transfer ID -> cancelled or already completed |
| UnbindController | Controller generation -> acknowledgement |

The broker itself uses the same abstract operations for transcript actions.
Internal calls are explicitly renderer-owned, not a fabricated application peer.
The common library separates these service operations from its 9P adapter so a
future Mycelium adapter preserves behavior, ownership and error semantics.

## 8. Wire framing, bounds and commit semantics

Use 9P2000.L negotiation and existing Rlerror errno transport. The interaction
file carries a request record with a fixed versioned header and explicit total
length. TWRITE chunks have contiguous offsets beginning at zero. TREAD returns
one response with the same request ID, at caller-specified offsets. One request
may be outstanding per interaction fid. No partial request takes effect. No
unsolicited server messages or polling at 100 Hz: use readiness and the existing
session/focus notifications. Mode updates coalesce before transmission.

HI-1 must commit matching Rust/C byte definitions and fixtures BEFORE clients:
magic HIN1, little-endian fields, exact header/body lengths, operation ID, u64
request ID, reserved fields zero; unknown versions/operations refused. Numeric
IDs and qid allocation must be checked against the existing protocol registry.
No pointer-sized fields, implicit struct padding or unbounded strings. 9P reads
and writes may split any record; the parser is an incremental bounded machine.

Initial limits, exposed by Hello and checked before allocating:

- Clipboard: 1 MiB UTF-8; no truncation. NUL rejected; CRLF/CR normalized to LF
  by text-export adapters before upload. Other nonprinting controls except TAB/LF
  rejected; Unicode format characters remain text and are not silently stripped.
- Copy/read transfer chunks: at most 16 KiB payload, within negotiated msize.
- Controller label: 64 UTF-8 bytes, plain text; protocol record: 32 KiB maximum.
- One active clipboard transfer per controller; two staged writes and two retained
  read snapshots per session, plus the current value: at most 5 MiB payload.
- 30-second inactivity expiry and 120-second total lifetime per transfer, measured
  monotonically. Eight request frames per connection per event-loop pass, with
  independent byte/time budgets so a stream cannot starve pointer or focus work.
- At most one active controller per live leaf: currently MAX_PANES = 32.
  Reserve 32 control connections, preserve two media-transfer connections, and
  allow four unbound handshakes, expiring after two seconds. Use eight fids per
  connection and aggregate 32 KiB input plus 32 KiB output budgets across all
  fids on a connection (not that allowance per fid). Cached results and partial
  frames count against those budgets; read replies reference pinned snapshots.
  With the
  clipboard payload limit above, the corresponding buffer ceiling is 7.375 MiB,
  excluding explicitly counted route/controller/transfer metadata. HI-1 must
  assert the complete allocation ledger and tie these limits to MAX_PANES.
  Do not eagerly allocate maximum buffers. A peer gets bound slots only for
  leaves it owns and one unbound handshake; full pools refuse promptly. The
  existing two-connection media policy cannot consume the control reserve.

Clipboard starts empty (generation zero). Commit validates full length and UTF-8,
rechecks foreground admission, compares the expected generation, and atomically
replaces the value. On failure, the previous value is intact. A generation
conflict is reported rather than silently overwriting a newer user's copy.
A successful response contains the new generation. An immediate retry of the
same completed request on the same connection returns its cached result, not a
second mutation. Retain the last result per fid until the next request. If the
connection dies after commit but before reply, outcome is unknown; reconnecting
clients must not blindly retry. They report the uncertainty and allow a new copy.

Reads pin an immutable clipboard generation at admission; concurrent copying
cannot concatenate two values. Sequential reads end with EOF; invalid offsets
fail. Copy-source exit after commit does not destroy clipboard contents. Failed,
expired or disconnected staging is discarded. Cancel/clunk before admission has
no effect; after publication it cannot undo a committed copy. No data is written
to disk; logout/session teardown clears the value and outstanding transfers.

Use existing errnos for denied, invalid, unsupported, too-large, busy/conflict,
timeout and gone/stale conditions; HI-1 pins exact existing values and the C/Rust
mapping. Never collapse all failures into an empty clipboard or successful copy.

## 9. Focus, lifetime and clipboard admission

The session is the sharing boundary. Same UID alone does not authorize a
background process to read or replace its clipboard. Registering a controller
requires the binding in section 3; GetClipboard and CommitCopy additionally
require its current keyboard-focused context. BeginCopy can reserve staging
only for a focused controller. Focus/context loss cancels unfinished writes.

Tapestry is the authority for focus and surface lifetime. A cached focus label
inside halcyond is insufficient for admission. Add a renderer-authenticated
focus-check request on the existing Tapestry control connection, ordered in the
same event loop as focus changes. Its reply binds a single broker operation to
session/controller/context generations and the current focus epoch. The renderer publishes controller/context ownership epochs through that same
ordered control connection before admitting interaction requests; a focus check
compares the stored epochs, not just values supplied in the request. The broker
matches it to that pending operation; it cannot be reused for another operation.
No new kernel syscall or transferable application credential is needed.

The focus-check's successful processing is the admission point. For CommitCopy,
all validation/allocation/generation checks happen before that request and a
short bounded publication follows its reply; conflicting clipboard commits are
serialized. A later focus change does not retroactively revoke a copy already
admitted. GetClipboard pins the generation as part of the pending admission;
an admitted read may finish after focus changes, within its bounded lifetime.
This explicitly permits completion, not fresh background reads. A stale reply,
dead controller or session teardown discards the pending operation. This order
must be represented in the implementation tests, not approximated by timing.

This is a foreground-application boundary, not a claim that a focused malicious
application is harmless. Boosty's privileged chrome is the clipboard participant;
web content processes receive no session endpoint/controller registration. The
browser mediates page clipboard APIs and user gestures under its own policy.
No application clipboard I/O or outstanding normal-seat admission is accepted
while a trusted SAK episode owns the seat. Entering that episode cancels pending
clipboard transfers; already delivered bytes cannot be recalled. Imperium-key
fields never publish clipboard data through this service.

## 10. Native pointer

Tapestry owns a display-coordinate pointer and the topmost normal-seat cursor
layer. The same coordinates and hotspot drive drawing and hit testing. The
software path is the portable baseline on CPU/GPU composition and future
framebuffer backends; VirtIO-specific cursor commands are optional acceleration.

Shapes: arrow (default), text beam, link hand, horizontal resize, vertical resize,
and hidden only during explicit pointer capture or a trusted seat transition.
Rasterize fixed theme-native vector shapes at HALCYON-SCALE, with contrasting
outline/fill across both bright and dark content. Define the hotspot per shape;
size is 24 logical pixels for the arrow's bounding box, scaled with the profile.
No arbitrary application cursor image upload in v1.

Only the owner of the live surface under the pointer may request its cursor
shape. Requests include the surface incarnation; retirement, leave or replacement
restores the default. Compositor-owned menu/divider state overrides client shape.
Pointer shape does not change keyboard focus or the mode widget. A stalled
client cannot stop pointer motion. Relative-capture requests use the existing
capture authority; on release/death restore a visible pointer at a clamped
position and stop delivering relative deltas to the retired owner.

Software drawing always composites the cursor over cursor-free scene pixels;
never save/restore stale application pixels under a moving pointer. Damage is
the union of old/new cursor rectangles plus ordinary content damage. Recompose
from current scene data, clip at display edges and update on shape/scale changes.
Coalesce motion to the newest position per frame, preserving button-event order
and the position associated with each click. No perpetual redraw while idle.

Direct scanout is eligible only if a separately qualified cursor plane can show
the pointer, or if the user has explicitly entered hidden relative capture.
Otherwise fall back to composition. Fullscreen ordinary applications, menus,
resizes and display-mode changes must not make the pointer disappear.

At SAK takeover, normal-seat cursor ownership ends before trusted presentation.
The trusted scene owns any cursor it displays; it may use its existing keyboard-
only interaction meanwhile, without accepting ordinary cursor requests. Resume
rebuilds the normal cursor from current seat state. Do not expand Lictor's input
or trust contract implicitly in this feature.

## 11. Implementation and delivery sequence

| Chunk | Scope | Completion witness |
|---|---|---|
| HI-0 | Portable pointer, standard shapes, damage and scanout fallback | Real captures/video; movement over changing content without trails; capture/death/resize tests |
| HI-1 | Wire ABI, controller ownership and focus admission, bounded session service, client adapters | C/Rust codec fixtures, malformed/partial I/O and lifetime tests; two-client copy/read demonstration |
| HI-2 | Mode reporting and Instrument widget; Nora clipboard bridge and Space menu | Real focus switching among shell/Nora/native test field; correct state after foreground exit |
| HI-3 | Transcript text anchors, motions, search, mouse selection, typed ut paste | Mixed Beacon/Unicode/soft-wrap tests, continuing-output and retention tests, no implicit command submission |
| HI-4 | Boosty native text-field adapter and clipboard chrome integration | Address-field copy/paste plus denied direct content-process access, coordinated with Main |
| HI-5 | End-to-end qualification, manual and Vault as-built updates | Complete workflow matrix and screenshots; no claims for unrun Pi hardware |

HI-0 can proceed without clipboard IPC. HI-1/2 must coordinate terminal foreground
ownership with current ut work; do not build a second job-control ledger. Resolve
collisions against Main's live tree before code. Preserve existing place clients
and measure /srv count before/after: zero additional registry entries per session.

Main owns Boosty's implementation. Share this contract before HI-1 ABI freeze.
If Boosty is not ready, a native text-field fixture qualifies the adapter but
HI-4 remains open; never label the fixture as a working browser integration.

## 12. Verification and release bar

Pure tests: modal transitions and prefix cancellation; Nora/transcript action
parity; UTF-8/grapheme hit tests; soft-wrap copy; search cancellation/wrap;
retention expiry; cursor geometry, clipping and damage; every protocol split and
malformed length; generation compare/commit; snapshot isolation; all bounds.

Lifecycle tests: background and other-session requests refused; same UID with
wrong controller refused; process/surface/context reuse; foreground job changes;
focus moving during admission; disconnect at each copy stage; response loss after
commit; logout, renderer restart and SAK takeover; resource pressure while two
image uploads proceed; no starvation of pointer or foreground input.

Graphical workflows: select proportional Beacon text by mouse, locally yank,
copy via Space y, switch to Nora, paste via Space p as one undo action; reverse
that route; paste multiline text into ut without running it; clipboard survives
source exit; address-field fixture rejects multiline input; APP fallback for a
foreign terminal application; all modes shown correctly under focus changes.

Run the affected host suites and actual QEMU interactive gates in the repository's
required SMP/build configurations under Yip leases. Capture the real cursor,
VIS selection, search, mode changes and Nora paste. Screen captures used as
pointer evidence must include the guest cursor, not the macOS/QEMU host cursor.
Test both composed paths and the direct-scanout fallback. Record render latency
and idle wakeups before/after instead of asserting 'smooth' from a still image.
Pi 400/500 qualification is a separate physical test row; an unrun row stays open.

Self-review is the currently authorized staffing; do not call it independent
adversarial review. Before release, update affected dossiers, registry mirrors,
audit-trigger documentation and an operator manual section explaining the local
versus shared register, selection/search and paste compatibility. Document only
implemented behavior in the shipped manual. No new formal-proof claim is made
by this prose specification.

## 13. Later Mycelium migration

Keep the service operations, session ownership, controller lifetimes, focus
admission, atomic copy and immutable read semantics. Replace connection setup and
record delivery behind the adapter. Do not expose SrvConn handles in application
APIs. Clipboard generations are service values, never 9P fids. Future handle-
bearing formats require their own content/ownership contract; text v1 does not
pre-allocate a fake SCM_RIGHTS mechanism. Consolidate duplicated transport code
in the separate Mycelium arc, as explicitly directed by the operator.
