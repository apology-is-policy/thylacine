# Halcyon interaction implementation

Approved specification: `docs/HALCYON-INTERACTION.md`. Astra owns this arc in
`codex/astra`, coordinating with Main and Aux through Yip. Single-agent
implementation and self-review, as the operator requested; no independent audit
is claimed. The separate user-authority drafts remain untouched.

## Current pickup (September 25)

Kernel qualification is committed as 5ad9ad27; the native observer client,
Halcyon host seal and real guest probe are committed as 3c3d3cbc. Full 50-boot
SMP/UBSan matrix, production --all and native HVF observer scenario pass within
the evidence boundaries recorded below. The staged-census refusal is resolved:
re-rendering after staging the new source made normal hooks pass. All four
separate drafts are byte-identical. No live clipboard endpoint is enabled.
The standalone readiness worker is committed as b2ce61a5. The next checkpoint
repairs explicit nonblocking media I/O and adds owned /srv watch registration;
source and verification are recorded at the end of this note. No service capacity
has been raised, and the worker is still not connected to Halcyon's UI loop. Next:
complete its activation obligations and connect Halcyon's service loop, then
host/Tapestry admission and clipboard integration. Do not rerun completed
kernel qualification without a relevant change. Await Aux's exact cleared TC-1a
SHA before touching his wire/lib/tile files. Current build artifacts are production
with HALCYON_SESSION=0; graphical verification requires the appropriate rebuild.

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

### Terminal ownership ABI checkpoint

The scope contract is committed as `8d59b072`. All three ABI mirrors now reserve
operations 16..21 and the 80/24-byte records. Compiled kernel C, libt C and Rust
fixtures match a literal 200-byte oracle on Linux/AArch64
(`work/hi1-pty-abi-pi-headers.log`). The first attempt failed before compilation
because the isolated Pi staging directory lacked kernel headers; source headers
were copied and the successful rerun retained separately. Pi was released.
No kernel operations or lifecycle hooks are implemented yet.

Self-review corrected the registration path: Halcyon is not a kernel binding
role, so it cannot read STATE. It sends its actual child PID with the binding
locator to Tapestry over the declared session connection. Tapestry reads STATE
as the observer and confirms that exact host before acknowledging registration.
This keeps the direct role gate intact. Binding IDs are also explicitly capped
at INT64_MAX to keep success distinct from negative errno results.


ABI source checkpoint: `b730a990`, on Astra only, with normal hooks passing.
The commit hook required the dispatch dossier as well as the ABI dossier;
both now describe the reservation accurately and no bypass was used. All four
preserved draft files were compared again with stash
`4f983db5dfc7aae852aa9debc8ebd9e90d73e1a5` and are byte-identical.

The contract now maps the exact existing source hooks and adds the necessary
post-unlock wake reference, bounded in-progress WATCH reservation, and
read-copyout recovery rules. These are implementation obligations, not completed
kernel behavior. Aux was asked for the cleared base on Yip 0108 turn 23; none
has been announced at this checkpoint. No resource lease or VM is held, and
no background automation was restarted for this new dependency.

## September 25: kernel ownership implementation

Aux cleared `0cb5b2443dce`; Astra integrated that exact base as `c252a7f3`
with normal hooks. The earlier waiting-for-base statements are historical.
SYS_PTY_REGISTER suboperations 16..21 now implement master/observer binding,
role-bound state and bounded watch Spoors, acknowledgement and fresh admission.
The combined check holds process lifecycle before pts. Successful foreground
changes invalidate old epochs; nominated membership/image changes do too.
Binder/observer exec/death and terminal teardown retire bindings before their
state can be reused. Watch allocation is outside locks, and reservations and
wake pins retain retired pool entries until every borrower is gone.

Three successive QEMU build/boot checks pass all 1698 registered kernel tests;
the last includes actual process exit/reap and actual image replacement before
exit. These new assertions extend existing pts entries because the separate
`test.c` authority draft is preserved. Logs: `work/hi1-kernel-{tests,refined,lifecycle}-build.log`,
`work/hi1-kernel-boot-first.log`, `work/hi1-kernel-{refined,lifecycle}-boot.log`.
The lean production compile also passes. External Alpine/clade fixture rows
skip explicitly; they are not coverage.

The existing pty/pty_stop clean and liveness configurations pass. Their six
mutants violate the expected named invariant/property. All four poll clean
configurations and seven named mutants pass the existing checker. Logs are in
`work/hi1-models/`, with the original and corrected harness summaries retained.
The local checker initially expected an invariant instead of a temporal-property
failure, then used the wrong TLC message spelling; the model itself consistently
reported `DeathWinsOverJobStop` as expected. The corrected checker matches that
exact name. No new model coverage is implied.

Self-review corrected several implementation details: epoch advancement on a
nominated process's image/group change prevents an old ACK reviving admission;
watch reads copy bytes without assuming destination alignment; generic navigation
clones cannot operate or release the original watcher reservation; malformed
high bits in pts IDs are refused before narrowing. The ENOSPC named by the design
was absent from the kernel registry; it is now pinned at POSIX 28 with Rust's
`NoSpace` conversion and display. The standalone native Rust mapping/sentinel
checks pass (`work/hi1-errno.log`). Review remains single-agent.

Still required before claiming this kernel checkpoint qualified: the complete
SMP/UBSan matrix, concurrent close/unregister/retirement stress, allocation and
counter-exhaustion cases, and actual syscall-front/usercopy integration. Before
exposing the clipboard: sealed host spawn, Control tag 7 (Aux owns tag 6),
authenticated host/session/Tapestry registration, matched focus admissions,
watcher-driven revocation, full broker accounting/cancellation and Nora/ut/Boosty
consumers. No live clipboard endpoint, new mode widget or new UI capture is
claimed by this implementation checkpoint.

The four authority/settings drafts remain byte-identical to
`work/hi1-sep25-preserved/`. After the Aux merge their two test files intentionally
differ from HEAD. `work/hi1-build.py` temporarily substitutes the index test files
for a build and restores the draft bytes in `finally`; use that preservation
pattern for all further build/render operations. Never stage those four files.

Five deliberate source regressions each produce their exact named FAIL:
unsealed BIND, binder-side nomination, delayed ACK after foreground change,
missing exec retirement, and missing death retirement. Evidence is in
`work/hi1-negative/` and `work/hi1-negative-resumed-summary.log`. The failing
fixtures short-circuit cleanup, and those negative boots subsequently reach the
harness timeout; their verdict is the named assertion, not a completed boot.
The original runner refused its first mutation because indentation did not match;
no mutant ran in that attempt. It restored and rebuilt the canonical source.
After all five actual mutations, the canonical rebuild/boot passes again.

A final build/boot after adding syscall malformed-operand/private-op refusal and
noncanonical pts-ID tests passes 1698/1698 (`work/hi1-kernel-final-{build,boot,uart}.log`).
The three compiled ABI mirrors also pass on this Mac using AArch64 ELF C objects
and native Rust (`work/hi1-pty-abi-mac.log`). Source review is recorded in
`HALCYON-INTERACTION-KERNEL-REVIEW.md`. Vault render/lint passes with no failures.
All these checks remain narrower than the still-required SMP and live-client work.

Source checkpoint: `97bf1077` on `codex/astra`, normal hooks passed. Mac was
released after 28 minutes. Re-requesting it for the full matrix returned WAITING,
position 3 behind Aux/Main; that is not permission to build. No Astra lease is
held, and the pointer heartbeat stays paused. Next resource command:
`yip hold mac 'HI-1 ownership SMP/UBSan matrix and remaining kernel qualification' --for 45m --wait 1s`.
This final pickup paragraph is a documentation-only working-tree update after the
checkpoint. Preserve it along with the four separate draft files.

## September 25: expanded qualification in progress

Tracked test defect HI1-Q1: the first expanded boot failed the native BIND
positive case (1697/1698). The new fixture reused pts_make_conn, which sets
server_stripes to zero; that helper is adequate for registry-only tests but
cannot represent a posted observer service. The frontend correctly refused it.
Fix the new fixture to provide the actual poster incarnation, then rerun the
whole boot. Evidence retained in work/hi1-qualification-{build,boot,uart}.log.
The concurrent retirement and counter-boundary assertions passed in that boot.

HI1-Q1 is fixed: the fixture now creates each SrvConn with its actual poster
stripes. Corrected Clang build and full QEMU boot PASS, 1698/1698, including the
native transport fronts, actual usercopy faults, WATCH handle-table failure and
retry, 256 concurrent retirement/unregister/last-close/rebind iterations, and
ID/epoch/revision exhaustion. Logs: work/hi1-qualification-corrected-{build,boot,uart}.log.
The first failed boot completed its remaining tests rather than leaking fixtures.
The old syscall-gate test's duplicate handle_table_alloc was also removed; proc_alloc
already supplies the table. No production behavior was changed by these test additions.

Before Mac availability, GCC/AArch64 syntax checks passed with and without
KERNEL_TESTS on Pi in /home/cora/projects/thylacine-astra-hi1-check; this was a
separate acquired/released lease, with no shared checkout or paired boot artifacts
modified. Evidence: work/hi1-pi-syntax-final.log (the missing-header staging failure
is retained separately). It is syntax evidence only. Mac was acquired through Yip
and released after the focused corrected build/boot. The authority drafts remain
byte-identical. Repeated SMP/UBSan, full production and positive EL0 workflows are
still owed. No new graphical UI or clipboard endpoint is delivered here.

The expanded regression checkpoint is 5ad9ad27 (normal hooks). A subsequent
45-minute Mac lease runs the complete ci-smp-gate matrix against images built
from that checkpoint, with both separate test drafts substituted from the index
and restored in the runner's finally block. Runner: work/hi1-matrix.py; log:
work/hi1-smp-matrix.log. It releases Mac on exit. Do not build/rebake these images
while the gate runs. HALCYON_SESSION=1, HALCYON_PROFILE=instrument, GOROOT=0,
MKFS_PRESERVE=0; omitted external toolchain fixtures are not coverage.

While those fixed images run, the native Rust pty_observer adapter is prepared
separately. BindingId is a checked locator, never a credential, and unbind is
explicit. Watch owns its read-only fd, integrates with PollSet and distinguishes
retirement from WouldBlock. The isolated AArch64 cargo check passes in
work/hi1-client-check (log work/hi1-client-check.log). The full matrix images do
not include this later library source. No application uses the adapter yet, and
positive EL0 transfers remain part of the live integration gate.

Halcyon's SessionTile spawn now sets T_SPAWN_PERM_SEAL explicitly; ordinary
slave-side children retain their default unsealed spawn. Isolated AArch64 checks
of libthyla-rs and halcyond pass (work/hi1-host-check.log). A new optional
kaua-term-probe --observer mode and tools/interactive/pty-observer.exp prepare
positive EL0 STATE and watcher coverage using real ptyfs/Tapestry services. The
probe supervises a sealed child, which verifies its ordinary child is unsealed.
It still needs its actual interactive run after the fixed-image matrix.
The initial probe compile caught a PollEvent/tuple mismatch and the HUP constant
name; these were corrected. An attempted root-directory cargo check failed to
find the vendored dlmalloc because the usr/.cargo configuration was not loaded;
logs are preserved. Use the usr working directory for the native checks.

The corrected probe check passes from usr/ using the vendored configuration:
work/hi1-observer-probe-native-check.log. The native probe remains unrun until
the image is rebuilt after the matrix. Aux was asked for the cleared TC-1a SHA
before Control-7 edits, retaining his wire/lib/tile ownership and tag-6 reservation.

Tracked integration constraint HI1-Q2: expanding PanePlaceServer's current two
connections to the approved 32 controller + 2 media + 4 handshake slots cannot
simply retain push_fds into Halcyon's main poll. In the design's upper bound,
32 terminal up-pipes + 38 service connections + listener + EventRing = 72 fds,
before pending terminal writes, above kernel POLL_MAX_NFDS=64. The current
session loop only caps down-write entries after appending all service fds.
No live expanded service exists yet, so this is an integration design constraint,
not a measured failure of the current two-connection service. Do not enable new
slots with a timer fallback or silently lower the approved capacity.

Next implementation must specify a bounded userspace transport worker (existing
poll/thread/pipe facilities) feeding the ordered UI loop, or another verified
aggregation mechanism. The recommended worker owns transport readiness only;
UI-thread clipboard ownership, Tapestry admission and revocation ordering remain
unchanged. Move request buffers through bounded queues without duplicating the
connection input/output allowance; count queue metadata and credits explicitly,
and prove wake, cancellation and shutdown/join behavior. Tapestry's own added
32 watcher fds fit its separate poll budget (two listeners + at most eight total
connections + 32 watchers = 42). No kernel poll-limit change is authorized or
implemented by this note. The operator was informed of this architectural issue.

## Complete repeated kernel matrix

The full ci-smp-gate finished with 50/50 PASS: default smp1/4/8 and UBSan
smp4/8, ten boots per row, zero corruption/external-kill/inject-miss/timing/other
in every row. Log: work/hi1-smp-matrix.log. The matrix used kernel checkpoint
5ad9ad27 and the previously baked userspace, before the later observer-wrapper
source; it is not runtime evidence for that adapter. The runner released Mac
after 32 minutes and restored both test drafts; all four protected files were
compared again and match exactly. A separate 20-minute Mac lease now runs the
production shape and positive native observer scenario via work/hi1-production.py.

## Production image and native observer checkpoint

The separate production runner completed successfully and released Mac after two
minutes. `tools/check-production.sh --all` PASS: lean joey, lean loginnable joey,
release/KASLR/hardened kernel and userspace, fresh paired ramfs/pool, and production
boot authentication for both development accounts. llvm-nm finds none of the three
KERNEL_TESTS fixture symbols in the production kernel. Logs are
work/hi1-production.log, work/hi1-production-symbols.log and
work/hi1-native-prodcheck.boot.log; full build log is build/prodcheck.full.log.

The rebuilt production image passes the new pty-observer interactive scenario
under HVF (one attempt, five seconds). A real sealed EL0 process mints a PTY,
opens the actual Tapestry service, binds, closes that temporary observer connection,
and exercises STATE, WATCH, poll readiness/consumption, duplicate-watch refusal,
last-watch closure/reopen, UNBIND, HUP/EOF and stale-ID refusal. Binder ACK/CHECK
are refused. The supervisor waits for successful child exit before printing PASS;
its ordinary child also proves seals were not inherited. The enclosing shell
still runs a pipeline after the probe. This verifies native BIND/STATE/WATCH/UNBIND
and ACK/CHECK role refusal; positive observer ACK/CHECK remains owed through the
live Tapestry integration. Logs: work/hi1-observer-native-runtime.log and
work/hi1-native-ls-ci-pty-observer.{log,steps}; accelerator evidence is copied to
work/hi1-native-ls-ci-timings.tsv.

All four protected drafts were compared again and match exactly. The current
build artifacts are PRODUCTION, HALCYON_SESSION=0; rebuild an appropriate image
before any boot-test or graphical gate. No Mac/Pi lease or Astra VM is left running.
The host spawn seal is compiled but has not yet been exercised through a Halcyon
tile launch; the native probe exercises the same spawn primitive independently.
No clipboard endpoint or mode widget is claimed, and no new UI screenshot exists.
Single-agent self-review covered typed pointer lifetimes, descriptor ownership,
explicit revocation, reserved roles, cleanup and probe success publication.

## Next source step while Mac is leased by Aux

HI1-Q2's concrete implementation review is in HALCYON-INTERACTION-READINESS.md.
The narrower recommendation aggregates readiness only: one bounded worker,
40-fd maximum, one UI wake descriptor, all protocol/admission/buffer ownership
on the UI thread. It explicitly retains the current terminal-write overflow
fallback and claims no new idle guarantee. Short-write retention, native worker
join/rollback, descriptor generation and wake-latch proofs precede activation.
No worker or expanded endpoint has been implemented. Aux still has no cleared
TC-1a SHA as of Yip 0108 turn 29, so Control-7 wire/lib/tile integration waits.

Final native-client docs are prepared, including the Processes manual's host
seal explanation. Manual checking passes for all eight installed sections. Vault render/lint
passes with zero failures and two existing warnings (sub-kernel-caps line citation,
48 stale dossiers). Mac was acquired for these checks and normal commit hooks;
no hook is bypassed. Current production artifacts precede
that manual paragraph and must be rebaked before claiming it is installed.


The first native-client commit was correctly refused by staged Vault lint:
view-code-coverage was rendered before the new files entered the Git index.
The renderer uses git ls-files, so the newly staged source changed its census.
Evidence: work/hi1-observer-commit.log. Re-render after staging the new source,
co-stage generated views, and retry normal hooks. All four drafts were restored
and Mac released on this failure; Astra is queued behind Main for the short retry.
This is a documentation checkpoint failure, not a failed production/native test.


## HI1-R1: readiness runtime qualification interrupted by boot failure

The first isolated Pi runtime image became extinct before login, so the new
worker probe did not run. Track this boot failure before any further feature
work; cause is not yet established. Preserve work/hi1-readiness-pi-runtime-first.log
and the isolated runtime attempt artifacts. The image used the preceding
production kernel and paired pool/ramfs, with only the probe ELF replaced in a
separate ramfs copy; compare an unchanged control image before attributing cause.

HI1-R1's first cause is established: missing baked snapshot made pool_restore
return without creating the per-attempt pool, although run_attempt always exports
that slot path. The unmodified control reproduced ENODEV and no block device.
The repaired fixture path passes its missing/coherent/stale snapshot, opt-out,
retry and partial-copy tests, and the next VM sees the actual 64 MiB pool.
That boot now reaches the backend and fails with STM_EBACKEND (-207), before
login. Track the second failure as HI1-R2. Linux QEMU advertises legacy MMIO by
default, and the boot also logs LegacyDevice for netdev; verify transport version
with unchanged artifacts rather than attributing the backend failure yet.

HI1-R2 is established as an emulator device-contract mismatch: Linux QEMU's
virtio-mmio force-legacy default is on; Stratum's backend explicitly refuses
version != 2 at initialization, returning STM_EBACKEND. Adding the modern
transport property to unchanged artifacts made the readiness probe PASS in KVM.
The launcher now pins that requirement on all hosts, with no guest-driver change.
The missing-pool fixture repair also passes its regression cases.

During negative-control qualification, the readiness scenario matched the FAIL
prefix and killed QEMU before the full reason reached its log. This truncated
the expected foreign-worker-token diagnostic and the outer verifier correctly
refused to count it. Track HI1-R3: match the entire failure line through its
newline, then rerun named controls and the restored canonical image. Preserve
the first truncated evidence in work/hi1-worker-mutant-owner-console.log.


## Standalone readiness worker qualification (September 25)

The new poll_worker native library is implemented, but not connected to Halcyon's
service loop. It owns bounded descriptor duplicates, one-shot interests and
worker/registration/arm identities, private coalesced notification pipes, a guarded
64 KiB stack and a kernel-confirmed join. File::try_clone uses existing syscall 12.
Compile-time bounds cover metadata, UI batch and owner; no protocol data or
clipboard decision is moved to the worker. The activated service still has only
two media connections. The approved expanded service remains unimplemented.

Cross-compilation and release linking passed on the leased Pi. The first probe
check found an i64/i32 timeout mismatch, corrected before runtime. Its isolated
source directory is /home/cora/projects/thylacine-astra-hi0; a fresh runtime
checkout is /home/cora/projects/thylacine-astra-hi1-runtime. The verified production
kernel and paired pool/ramfs were copied there, and a separate ramfs replaces ONLY
kaua-term-probe. The original pair and kernel hashes stayed unchanged. This is
native adapter evidence against the production kernel, not a new full-image bake.

The final native readiness scenario PASS in KVM: 39 live service slots,
63-slot construction, already-ready sources, one-shot disarming, re-arm, independent
peers, 64 descriptor close/reuse cycles, cross-worker token refusal, joined shutdown,
and setup rollback after exhausting handles with only three slots available.
The shell runs a pipeline afterward. An earlier clean pass, two named mutants
(disarming removed / worker-ID check removed), restored clean pass, and final
formatted/allocation-asserted source pass establish the controlled sequence.
Mutants fail on their specific complete diagnostics, without kernel extinction.
Self-review caught and fixed cross-worker token collision before activation.

Evidence: work/hi1-readiness-final-{build,runtime,console}.log,
work/hi1-readiness-variants-complete-lines.log, and
work/hi1-readiness-{canonical,mutant-repeat,mutant-owner,restored}/ containing source,
ELF, image digests, build and console logs. The initial failed compilation,
boot/control failures, and truncated mutant diagnostic are retained separately.
HI1-R1/R2/R3 are resolved by the pool fixture, explicit modern MMIO transport,
and complete-line matcher fixes. tools/test-ci-pool.sh passes on both Linux and
macOS. No new visual UI or screenshot is claimed.

Before connecting and activating the persistent service, still owe deterministic
adversarial wake/rearm schedules, remaining allocation/dup/spawn failure injections,
short-write response retention and the full 38-connection IPC/admission ledger.
Then integrate Halcyon's service loop and host/Tapestry control flow; Aux's
TC-1a-cleared SHA still precedes Control-7 wire/lib/tile edits. This checkpoint
is single-agent implementation/self-review, not an independent audit.


## HI1-R4: accepted media service can block the compositor

Source inspection after b2ce61a5 found that paneplace assumes nonblocking server
I/O without setting CNONBLOCK. devsrv_write now uses
srvconn_server_send_blocking unless the accepted Spoor is marked nonblocking;
byte-mode server reads can block too. A stalled reply reader can therefore park
the UI. This also invalidates the readiness review's claim that the default
server endpoint supplies nonblocking I/O. Fix before service activation: mark
every accepted connection nonblocking, retain short writes/WouldBlock, and
exercise stalled-reader progress and buffered-frame continuation. No failure
has yet been reproduced at runtime; this is a source-established blocking path.


## HI1-R5: first service probe hit SAK before pending request

The first service-wire native run did not execute the probe: after the test
matched imperium's `as pid` prefix and immediately sent BREAK, Corvus displayed
`nothing pending`. Preserve work/hi1-service-runtime-first.log and its console.
Determine the request/arming order and synchronize the test with a real armed
state rather than treating this as a timing flake. Compilation issues in the
first adapter/probe builds were corrected; both release binaries now link.

HI1-R5 is a fixture mismatch, not an established arming race. Corvus dispatches
the request before acknowledging the final Twrite. The lean joey provisions
login accounts but no imperium clearance/key; `imperium --list` lists only the
automatic JIT tier. A control run without BREAK confirms `imperium: not eligible
for the imperium level` (work/hi1-service-fixture.log). The first diagnostic
incorrectly expected zero eligible levels and failed on that assertion; its
log is retained as work/hi1-service-fixture-first.log. The gate now requires
the actual imperium enrollment before requesting SAK. A separate isolated
hi1-service-runtime copies the September 24 CI ramfs and matching pristine pool
(key snapshot compared equal), plus the already-qualified production kernel.
Only its probe ELF is replaced; original production/CI sources remain unchanged.


## HI1-R6: native service probe setup refused after enrollment

The corrected isolated fixture reaches successful POST_SERVICE conferred, but
the probe's generic open/post/accept setup check fails. Preserve the enrolled
run and add operation-specific errno diagnostics before inferring a cause. The
CI-session-on attempt was stopped through its own QMP socket after it proved
that fixture launches graphical login; the final fixture uses the original
production key/pool/config with only the existing CI joey and new probe replaced.

HI1-R6 is the probe supplying POSIX mode bits to a service POST. Its diagnostic
reports `post byte service returned -22`; sys_srv_post_perm_ok explicitly
permits only DMSRV transport bits. Removed the erroneous 0600, retaining the
existing scope/principal/TCB-dial gates. No production kernel change is needed.


## HI1-R7: readiness duplication conflicts with /srv ownership

After setup correction the actual SrvConn pump reaches its first readiness
registration and fails: `watch blocked writer`. Source confirms
handle_dup_common refuses every devsrv Spoor and KObj_Srv (NoSrvSpoorDup /
SrvHandlesAtOrigin). Pipe-only qualification did not cover the intended service
handle class. Preserve work/hi1-service-runtime-canonical.log. Do not weaken the
kernel alias/identity contract. Add explicit owned registration: consume a File
into the worker, borrow its raw descriptor only during a closure under exclusive
owner access, release the state mutex before I/O, and retain the owned descriptor
until removal is observed after poll. This adds no allocation or kernel ABI.
The existing duplicated registration remains useful for transferable sources.


## HI1-R8: combined probe assumed listener-close unposts

The owned-watch transport checks now reach completion, then the second, actual
media adapter POST fails. The probe assumed dropping a listener frees its
service. KObj_Srv release is intentionally a no-op: service lifetime is the
poster Proc, and this checkout still has a 16-slot registry. Run the standalone
transport portion in a child and join its exit before posting the media adapter;
this honors real registry lifetime and permits cap-slot recycling. Retain the
failed combined log; a passing rerun must still prove the actual media path.

The poster-process split allows the media POST. Its first clients then fail
before accept: the test incorrectly tried a flat /srv/service/token/place open.
The real view client first opens /srv/service (instantiating its kernel 9P
client), then opens token/place relative to that handle. Correct the probe to
use this existing contract, and retain work/hi1-service-media-open-first.log.
This is HI1-R9, a test-client setup error, not a service transport verdict.


## Nonblocking media transport and owned /srv watches (September 25)

HI1-R4 is repaired in both console and session adapters: accepted endpoints must
successfully enable nonblocking mode before Conn publication. Shared servicewire
retains one reply buffer/offset across short writes and EAGAIN, dispatches each
request once, caps each turn at eight frames and 64 KiB I/O, and observes a shared
two-millisecond service-pass deadline. Connection order rotates. Complete buffered
frames explicitly keep the UI runnable; partial input/blocked output wait on
READ/WRITE without timer polling. Conn/server handles close on Drop; registry
unposting remains process-lifetime, not handle-lifetime. Media capacity stays 1/2.

HI1-R7 changes the internal worker adapter, preserving the kernel NoSrvSpoorDup
contract. register_owned consumes a File with no duplication/allocation. with_fd
borrows it under exclusive access to the owner, after releasing the state lock;
remove/shutdown cannot run during that closure, and the worker never closes a
live slot. Retired IDs refuse I/O. An owned accepted endpoint is closed after
kernel-confirmed worker join. Transferable-source register still uses SYS_DUP.

The final service-wire native gate PASS on the isolated Pi/KVM: explicit
POST_SERVICE conferred; full real SrvConn reply ring; unrelated peer progresses;
17-byte drain creates exact short-write credit; readiness wakes then disarms;
five 32 KiB replies arrive with exact ordered bytes and no duplicate dispatch;
owned listener/connection retirement and join; then the production PanePlaceServer
source handles two child processes through real kernel 9P clients, each uploading
a 256x256 image whose ID, target leaf and every ARGB pixel are checked. Both
children exit successfully and a shell pipeline runs afterward. This compiles
the actual adapter into the probe; it is not a graphical image/rendering test.

The isolated fixture is /home/cora/projects/thylacine-astra-hi1-service-runtime.
Its production kernel, original paired production ramfs/pool/key/config are
unchanged; the separate runtime ramfs replaces only kaua-term-probe and the
previously-built CI joey, needed for test enrollment. The original CI session-on
pair is preserved under work/ci-session-on-base. These fixture differences and
failed runs are recorded in HI1-R5/R6/R8/R9 above; no passing claim is drawn from
them. Final native evidence is work/hi1-service-complete-runtime.log plus the
console and artifact digests under work/hi1-service-evidence/. All 341 Halcyon
host tests pass. The lost-offset and suppressed-buffered-work mutants each fail
their intended named test; all six restored pump tests pass. Both final native
release binaries link, and the existing native readiness gate passes again after
the owned-registration refactor (9 seconds). The final service gate takes 43
seconds. final-provenance.txt compares the original production and service base
kernel, ramfs and pool byte-for-byte and checks that only joey and the probe differ
in the runtime ramfs. No kernel source changed in this checkpoint; the earlier
50-boot matrix was not repeated. Pi is released. Normal Vault render/lint and
commit hooks await the Mac lease; the source is not yet committed or on Main.

Still owed: readiness failure/interleaving gates, full 38-slot admission/IPC
ledger, connection of the worker to Halcyon, Tapestry/host Control-7 admission,
clipboard clients and visible modes. Fatal worker failure after posting cannot
be handled by merely closing a listener; before activation verify compositor
exit and session recovery. Aux has not supplied a cleared TC-1a SHA. No new
screenshot or live clipboard is claimed. This remains single-agent self-review.

### Pending commit pickup

The 24 source/prose paths are staged. work/hi1-service-staged-manifest.json records
the exact base and hashes. The separate heartbeat
finish-hi-1-transport-checkpoint-after-yip-lease runs every five minutes to maintain
the Mac queue and finish this checkpoint; the old pointer heartbeat remains paused.
Run python3 work/hi1-service-commit.py from Astra: WAITING/exit 75 only refreshes
queue position. Successful hold permits render/lint and normal git hooks; finally
restores the four protected drafts and releases Mac even on failure. The script
refuses changed source/index/base rather than committing unrelated work. Successful
commit writes work/hi1-service-committed.json; verify it and pause the heartbeat.
Do not re-run the completed native/host gates solely because the lease was delayed.
The runner's post-acquisition failure requires inspection before a retry, especially
if render has staged generated views. Do not bypass the hooks or reset any drafts.


### HI1-R10: checkpoint render refused a malformed Record link

The first Mac slot reached Quaestor render, which found a dangling combined
wikilink in the interaction arc. The source edit had unintentionally changed
both the chunks frontmatter and an existing body link. Restore that body link
exactly, preserving the append-only Record contract; the new chunk stays in
frontmatter and the appended transport section. The failed render log is retained
as work/hi1-service-vault-render-first.log. Generated views remain unstaged until
the corrected render/lint passes. The lease was released and all four protected
drafts restored byte-for-byte. No source or runtime test result changed.
