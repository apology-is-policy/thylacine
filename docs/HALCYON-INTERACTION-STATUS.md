# Halcyon interaction implementation

Approved specification: `docs/HALCYON-INTERACTION.md`. Astra owns this arc in
`codex/astra`, coordinating with Main and Aux through Yip. Single-agent
implementation and self-review, as the operator requested; no independent audit
is claimed. The separate user-authority drafts remain untouched.

## Delivered source checkpoint

HI-0 provides the five standard 24-logical-pixel pointer shapes, scale-aware
geometry/rasterization, surface-owned shape preferences, Tapestry hover/divider
selection, and Lictor's private VirtIO cursor plane. Operation 65 accepts only a
standard shape, supported scale, position and visibility. The cursor queue has
one outstanding chain with checked retirement and retained DMA on ambiguity.
Trusted takeover replaces the cursor with transparent pixels and hides every
output before acknowledging exclusion; restoration republishes normal state.
Cocoa's forced host pointer is disabled by default.

HI-1a provides the HIN1 envelope, exact request/response bodies, error mapping,
canonical clipboard-text validation, a bounded fragmented receiver and twenty
matching C/Rust fixtures. There is no clipboard
endpoint, controller registration, Nora bridge or mode widget
change yet. See `HALCYON-INTERACTION-ABI.md`.

## Verified on September 24

- Main's verified `5857b6bf` was integrated as `08c26509` with normal hooks.
  This includes B-1c, the Aux seal/HN-1 merge, both lantern and heap-probe, and
  the current native allocator. No authority drafts entered those merges.
- The CI boot image built successfully (`work/hi0-build.log`). The build script
  temporarily used the index versions of two authority test files, then restored
  their exact draft bytes. All four preserved draft files match the saved stash.
- 274 current-source Linux/aarch64 host tests pass: libhalcyon 141, libtapestry
  10, Lictor 28, Tapestry 95 (`work/hi0-pi-host-current-main.log`). Earlier
  Thylacine cross-checks and release links also pass, as does the native C wire
  fixture. Earlier logs remain under `work/hi0-pi-*` and `work/hi1-pi-tools.log`.
- `ls-halcyon-pointer` passes in QEMU/KVM at 1280x800, scale 100, with both
  virtio-gpu-pci and virtio-gpu-gl-pci. The latter has a real VirGL 3D compositor
  surface on the Pi GPU. Five shapes, both edges, divider selection, owner exit,
  SAK transparent exclusion and restoration are witnessed. Both boots report
  1691/1691 kernel tests. Evidence: `work/hi0-{2d,gl}-pass.log` and
  `work/hi0-pi-runtime-{2d-fixed,gl}.log`.
- Eight raw scene captures per backend retain the exact flat fixture pixels,
  without cursor pixels/trails (`work/hi0-pi-pixels.log`). That assertion is now
  also in the capture harness. Real captures are under `work/hi0-{2d,gl}`.
  Each includes the raw framebuffer, actual VNC AlphaCursor plane and a JSON
  witness. The composite uses the guest-acknowledged position because QEMU VNC
  does not transmit pointer position. No host pointer or replacement mockup.
- All three accelerated graphical SAK scenarios pass: `ls-graphical-sak`,
  `ls-graphical-sak-states`, and `ls-graphical-sak-recover`. These cover real
  authority witnesses, interrupt/abdication, denial/cancellation, true expiry,
  five-failure lockout, held-chord failure and recovery followed by conferral.
  Logs: `work/hi0-pi-sak-gl.log` and `work/ls-ci-ls-graphical-sak*.log`.
  Actual captures: `work/hi0-sak-gl`.

The separate KVM boot/probe gate passes (`work/hi0-pi-boot-gate.log`). Its
production compile row skipped because this isolated runtime staging lacks
`build/generated`; external Alpine/clade rows also explicitly skipped. Those
are not coverage. The final harness run includes the newly embedded raw-pixel
assertions and passes (`work/hi0-pi-runtime-gl-final.log`,
`work/hi0-gl-final-pass.log`, captures `work/hi0-gl-final`). These results
qualify the named composed paths, not all HI-0
requirements or all production configurations.

## Architectural findings and remaining qualification

Tapestry's CPU mirror is incomplete for GPU-only content. A software cursor
cannot safely erase/restore from that mirror. VirtIO uses its hardware cursor
plane; future software backends must supply a complete cursor-free scene and
proper damage integration. QEMU VNC ignores visibility alone, requiring the
transparent replacement before hide. Queue scratch, image and controlq storage
are disjoint. Images reach cursorq only after checked controlq upload.

Cursor waits retain the longer controlq readback allowance even if readback
retires first: both queues share the device loop. This prevents a false timeout;
input latency under load remains unmeasured. Coordinates follow current display
geometry rather than stale boot EDID. App preferences die with surface ownership.

Still open: explicit application relative-pointer capture and its client
migration (relative motion is not capture); direct-scanout qualification;
additional scales/modes and failure-injection matrix; latency/idle measurement;
production-feature and full sanitizer qualification; Pi bare-metal backends.
The existing `ls-gfx-panes` direct-scanout harness assumes the legacy console
image and hard-codes HVF, so the Halcyon image cannot honestly substitute for
that row. `docs/manual-drafts/19-pointer.md` remains a draft, not installed help.

## Corrected failures

HI0-H1: the first pointer run expected an arrow at (640,400) after the demo
closed. The restored split correctly puts its divider there. The fixed harness
asserts resize-h, then moves into the right tile and asserts arrow. Both 2D and
VirGL reruns pass. Original failure: `work/hi0-2d-h1-failure.log`.

The first invocation used the abbreviated scenario name `halcyon-pointer`,
which was refused before boot. Use `ls-halcyon-pointer`. Earlier isolated Pi
checks initially lacked theme fixtures and used string comparisons for byte
argv; both were corrected and the failed evidence retained. During the Main
merge, an old compiled Quaestor produced a stale coverage view. Rebuilding the
helper from the current source and rendering again passed normal hooks.

## Preservation and pickup

Permanent worktree: `/Users/northkillpd/projects/thylacine-astra`. Isolated Pi
work directory: `/home/cora/projects/thylacine-astra-hi0`; Rust is under
`/home/cora/.cargo/bin`. The agents' shared Pi checkout was not changed. Paired
boot artifacts were SHA-256 verified (`work/hi0-artifacts.sha256`).

Never stage all files. Preserve `.claude/settings.json`,
`docs/USER-AUTHORITY-DESIGN.md`, `kernel/test/test.c`, `kernel/test/test_devproc.c`
and unrelated `work/` evidence. Safety stashes remain referenced by
`work/hi0-main585-stash.txt` and `work/hi0-merge-stash.txt`; the earlier patch is
`work/hi0-before-merge.diff`. `work/hi0-compile.py` preserves the authority test
files for builds. The compiled `work/quaestor` now matches current source.

HI-1 next connects the pure clipboard store to
authenticated controller ownership and focus integration. HI-2 adds mode reports
and Nora's Space-y/p bridge. HI-3 adds transcript motions/search/typed paste.
HI-4 joins Main's Boosty fields; HI-5 closes workflows and manuals. No extra
9P registry slot or Mycelium dependency is introduced.

The queued verification is complete. Pi was released after 20 minutes, with no
QEMU left running. The heartbeat `resume-halcyon-after-yip-lease` is PAUSED as
instructed, so it will not silently reacquire a resource after this checkpoint.
Use fresh Yip leases for the next implementation/build phase.

Implementation and evidence checkpoint: `12af4d53` on `codex/astra`; this is
not a landing into Main. Normal hooks pass. The new pointer harness is declared
in the boot-banner mirror registry, and generated views were rendered after
adding the new source files to the index. The separate authority/settings drafts
remain outside the checkpoint and still match the saved stash bytes.

## HI-1a typed protocol checkpoint

All 148 libhalcyon tests and the independent C encoder's twenty frozen wire
vectors pass on Linux/aarch64 (`work/hi1a-pi-all.log`). Initial optional rustfmt
invocation on Pi found no formatter and stopped before testing; the corrected
run passed, and local rustfmt changed whitespace only. Pi was released.
The receiver's allowance is supplied by its future owner; aggregate accounting,
replay, ownership/focus checks and storage are not implemented by this codec.
Source review remains single-agent, not an independent adversarial audit.

The existing Halcyon session is a declared Tapestry session connection, not a
kernel console-renderer peer. Focus integration must authenticate that exact
declared connection and its hosted leaves, rather than require the unrelated
console-renderer flag or accept any same-principal client. PTY foreground changes
also need the approved ordered bridge; periodic pgrp sampling is insufficient.

## HI-1b bounded storage checkpoint

`usr/halcyond/src/clipboard.rs` supplies pure storage, not a live service. It
bounds current/staged/pinned payloads at 5 MiB, checks exact owners and session
generations, and separates prepare from admission completion. Reads pin before
the check; commit validates/freezes before the check and publishes without
allocation. Stale tickets cannot revive cancelled work. Absolute and idle expiry
use a next-deadline API for event-loop integration. The complete connection and
metadata ledger remains pending with the broker.

The initial Pi compile lacked this isolated checkout's IBM Plex fixture files;
no tests ran in that attempt (`work/hi1b-pi-tests.log`). After copying the pristine
fixtures into the isolated directory, 334 Halcyon tests passed. A ninth storage
test then added the deadline wakeup/refusal checks; final evidence is recorded
below. No shared Pi checkout or paired boot artifact was changed.

Foreground integration is still under review: kaua-term's existing wire has no
explicit foreground-owner notification, and ut's job-control path currently
ignores set-foreground errors. Main has been notified before any edits there.
The bridge must acknowledge actual handovers; terminal output and sampled pgrp
are not substitutes for the approved ownership contract.

Final storage validation: 335/335 Halcyon library tests pass on Linux/aarch64
(`work/hi1b-pi-deadline-fixed.log`), including nine storage tests. The preceding
335-test run failed the new deadline fixture because it supplied time 150 after
time 200; regressing time correctly expired the write. Reordering the fixture
timestamps fixes the test without weakening expiry. Failure evidence remains
`work/hi1b-pi-final.log`. Pi is released, no VM was started.

## Terminal ownership scope decision pending

Source tracing found that `pts_tty_set_fg` permits any controlling-session member
to change foreground group. A shell-only notification can therefore leave the
broker's controller record stale; independently sampling GET_FG does not make it
current at Tapestry admission. The existing terminal host also does not request
a seal at spawn. No terminal clipboard endpoint has been exposed.

`HALCYON-INTERACTION-PTY-REVIEW.md` records the source-derived counterexample,
Plan 9/POSIX/Wayland/Fuchsia/Genode prior art, and two viable directions: expand
the kernel pts seam for lifetime-bound ownership observation/admission with a
sealed host, or defer terminal clients and continue graphical clients first.
The operator has been asked because the approved design excluded new kernel IPC
mechanisms. This is a proposed scope change, not ratification or code. Main and
Aux have been notified on Yip; no existing authority drafts were changed.

Current source checkpoints are `6e596d19` (typed HIN1 protocol) and `cb02b748`
(pure storage), both on `codex/astra`, with normal hooks passing. Neither is a
Main landing or a live clipboard. No resource lease is held. The earlier pointer
heartbeat remains paused; it is not a background worker for this new scope.

### Scope approved

The operator selected "Expand to kernel-backed ownership (recommended)".
Continue with the narrow pts observation/admission seam, sealed terminal host
and preserved ordinary job control. No further scope permission is needed for
that direction. Pin the concrete ABI, process/pts lock order, readiness and
revocation lifecycle before implementing its consumers; coordinate kernel
surfaces with Main/Aux. The pending-decision paragraphs above describe the
preceding checkpoint, not the current authorization.

Concrete contract: `HALCYON-INTERACTION-PTY-ABI.md` reserves SYS_PTY_REGISTER
suboperations 16..21, an 80-byte state record and 24-byte ACK/CHECK input. No
new syscall number; Main's pending 126 remains his. Aux agreed Control subtag 7
for the host binding announcement (his ScreenErased is 6). His Yip turn 20
requires taking the newly cleared aux-3 tip after H3+C's sabotage/SMP evidence;
that SHA is not announced yet. Main's stable tip is 13607e58 (docs only). Keep
proc lifecycle changes pending that base; ABI/pure source preparation can proceed.
