---
id: arc-halcyon-interaction
type: arc
title: "Halcyon interaction: pointer, clipboard and modal text"
status: active
design: [docs/HALCYON-INTERACTION.md]
chunks: [chg-2026-09-24-hi0-pointer, chg-2026-09-24-hin1-envelope, chg-2026-09-24-hin1-bodies, chg-2026-09-24-clipboard-storage, chg-2026-09-24-pty-interaction-abi, chg-2026-09-25-pty-interaction-kernel, chg-2026-09-25-pty-interaction-qualification, chg-2026-09-25-pty-observer-client, chg-2026-09-25-readiness-worker]
follow-ons: []
exit-criteria:
  - "[ ] Portable guest cursor works over composed/fullscreen content and capture transitions"
  - "[ ] Bounded session clipboard preserves local registers and enforces controller/focus admission"
  - "[ ] Focused context mode appears correctly in the existing Instrument status widget"
  - "[ ] Nora and transcript share navigation, selection, search and explicit clipboard actions"
  - "[ ] Native paste inserts text without submitting a shell command"
  - "[ ] Boosty native text-field integration is verified on the real browser"
  - "[ ] Runtime workflow evidence, screenshots, operator manual and as-built dossiers are complete"
created: 2026-09-24
---
## Goal
Deliver the interaction direction recorded in [[dec-2026-09-24-halcyon-interaction]].
`docs/HALCYON-INTERACTION.md` is the approved specification. HI-0 is starting. Authority work and its saved drafts remain separate.

## Planned chunks
HI-0 pointer; HI-1 controller and service contract/ABI; HI-2 mode widget and Nora
clipboard bridge; HI-3 transcript selection/search and typed paste; HI-4 Boosty
integration with Main; HI-5 qualification and operator documentation.

The transport is the existing session 9P service extended with bounded hand-built
interaction records. No Mycelium dependency or additional per-session /srv slot.
The new document identifies current paneplace connection limits as a design
constraint to resolve before persistent application clients are added.

## Close summary
Open. Full specification approved for implementation on 2026-09-24.
No code, runtime verification or new UI captures belong to this arc yet.

## Implementation checkpoint, September 24

HI-0 now has source for the standard pointer shapes, private Lictor cursor
plane and trusted exclusion, plus the capture harness. HI-1 has an envelope
foundation and C mirror. The Pi host suite passes 274 tests. Source remains
uncommitted while boot-image and actual UI qualification wait on the coordinated
Mac lease; the earlier close-summary sentence describes ratification time.
No new UI screenshot or completed clipboard workflow is claimed yet.

## Runtime checkpoint, September 24, 16:18 UTC

The heartbeat acquired Mac, completed Main reconciliation through `5857b6bf`
as `08c26509`, built the CI image and released Mac. The separate authority
drafts match their saved stash bytes. Under a Pi lease, the isolated QEMU/KVM
runs pass the full pointer scenario with 2D and VirGL 3D composition. Real VNC
framebuffer/cursor-plane captures verify the visible shapes and transparent
SAK exclusion; guest ctl provides their acknowledged positions. The broader
graphical SAK state/recovery scenarios are still running. Clipboard service,
application capture, direct scanout and wider qualification remain open.

### Completed queued runtime checks

The three accelerated SAK scenarios subsequently passed, including real expiry,
lockout and recovery. Refreshed current-source host tests pass 274/274 and the
separate boot/probe gate passes. Its skipped production/external-fixture rows
remain unqualified. The heartbeat's resource wait succeeded; the larger
interaction implementation remains open beyond this pointer checkpoint.

The final pointer harness, including the embedded raw-pixel assertions, also
passed on VirGL. Pi was released with no VM left running and the queue heartbeat
was paused after completing its requested runtime verification. Captures and
remaining obligations are indexed by `docs/HALCYON-INTERACTION-STATUS.md`.

## Typed ABI checkpoint, September 24

HI-1a now pins all operation bodies and existing-errno failures, with a bounded
fragment receiver and twenty frozen C/Rust vectors. The 148 library tests and C
fixture pass on the isolated Pi host. This remains protocol groundwork: no live
clipboard endpoint, mode widget or new runtime workflow is claimed.

## Storage checkpoint, September 24

HI-1b adds the pure bounded clipboard store and deferred read/commit admission.
The endpoint is still absent; focus/controller authentication is explicitly left
to the next broker integration. Atomic publication, snapshot isolation, stale
owners, SAK cancellation, deadlines and payload limits have dedicated tests.

## Terminal ownership design gap, September 24

Before connecting terminal clients, source review found that kernel SET_FG can
be called by other controlling-session members, so notifications from ut alone
cannot maintain the approved authority record. GET_FG exposes no epoch and an
independent sample races graphical admission. The proposed correction is in
`docs/HALCYON-INTERACTION-PTY-REVIEW.md`, with primary-source prior art and the
alternative of deferring terminal integration. Operator scope approval is
pending because the original design excluded new kernel mechanisms. No kernel
change or live clipboard endpoint is implied by the completed protocol/storage
checkpoints (`6e596d19`, `cb02b748`).

### Operator approval of the terminal correction

The operator selected "Expand to kernel-backed ownership (recommended)" on
September 24. The narrow pts ownership/admission extension and sealed host are
now in scope. The preceding pending-decision paragraph is historical. Numeric
ABI, process/pts ordering and bounded readiness/lifetime details must precede
their consumers; ordinary PTY job control remains intact.

The concrete observer contract is `docs/HALCYON-INTERACTION-PTY-ABI.md`: six
suboperations on SYS_PTY_REGISTER, bounded pollable watchers, lifecycle-before-pts
checks and no new syscall number. Aux reserved terminal Control tag 6 and agreed
Astra tag 7. Lifecycle implementation waits for his announced cleared H3+C base;
no raw kernel/terminal code is inferred from this design checkpoint.


### Terminal ownership ABI reservation

After the operator's scope approval, the detailed contract was committed as
`8d59b072`. Kernel/libt/Rust definitions and a compiled three-way byte fixture
now pin the reservation ([[chg-2026-09-24-pty-interaction-abi]]). This adds no
live syscall operation. Tapestry verifies the host identity in its observer role;
Halcyon receives a registration result, not broader STATE access. Aux's cleared
lifecycle base is still required before the kernel implementation.

## Kernel observer checkpoint, September 25

Aux's cleared `0cb5b244` was integrated as `c252a7f3`. The pts ownership operations,
bounded watch lifetime and process hooks are implemented on Astra. Current QEMU
regressions cover role/epoch checks, capacity and actual exec/death retirement;
existing PTY/poll models and mutants behave as claimed. See
[[chg-2026-09-25-pty-interaction-kernel]] and the phase status for coverage limits.
Full SMP qualification and all live userspace consumers remain open. The separate
authority/settings drafts are preserved, and this is not a Main landing.

## Expanded kernel regression checkpoint, September 25

The real transport frontend, faulting native usercopy, WATCH allocation rollback,
concurrent retirement and all counter boundaries now run in the kernel suite.
The corrected full boot passes 1698/1698; the initial fixture refusal is retained
as evidence rather than erased. See [[chg-2026-09-25-pty-interaction-qualification]].
Repeated SMP/UBSan and live userspace integration remain open.

### Native observer adapter

[[chg-2026-09-25-pty-observer-client]] adds typed calls and owning, pollable watch
handles over the pinned kernel ABI. AArch64 checks and the full
production/native probe pass. The preceding kernel checkpoint also passes all
50 SMP/UBSan boots. Positive observer admission and application clipboard/UI
integration remain open; see the status for the separate evidence boundaries.

### Standalone readiness worker

[[chg-2026-09-25-readiness-worker]] adds the bounded native aggregation layer and
records real descriptor tests plus negative controls. Halcyon service connection,
expanded persistent IPC and application clipboard workflows remain open.
