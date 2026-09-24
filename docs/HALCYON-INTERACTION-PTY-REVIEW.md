# Terminal ownership gap in Halcyon interaction

Status: design review, not approved and not implemented. September 24, 2026.
This is a scope correction to HALCYON-INTERACTION sections 3 and 9, whose
foreground ownership guarantee cannot be implemented by shell notifications
alone on the current kernel interfaces. The HIN1 ABI and pure clipboard store
are independent and already committed as `6e596d19` and `cb02b748`.

## What the tree actually supplies

- `kernel/pts.c::pts_tty_set_fg` allows a member of the controlling session to
  choose a live group in that session. It does not require the shell to be the
  caller and does not notify Halcyon. `pts_tty_acquire` also changes foreground
  state. The other assignments clear it during registry lifetime changes.
- `pts_tty_get_fg` returns a numeric group, not an ownership epoch or a binding
  to the process incarnation requesting clipboard access. A master can query
  it, but an independently sampled value cannot prove it is still current at
  Tapestry's later focus admission point.
- `usr/utopia/libutopia/src/eval/stmt.rs::run_foreground_jc` calls the setter
  around a job, ignoring its return values. Other same-session processes can
  call the syscall directly. A shell-only notification misses those paths.
- `usr/kaua-term/src/main.rs` owns the PTY master. Its Halcyon pipes carry
  terminal records/input, not an authenticated foreground-ownership protocol.
- `usr/halcyond/src/session.rs::SessionTile::spawn` does not request the seal
  for kaua-term. `Command` defaults permission flags to zero. If kaua-term is
  promoted into an authority-bearing controller registrar, protecting only
  halcyond is insufficient: the host and its bridge must have their own seal
  and explicit lifetime, rather than relying on ordinary same-user isolation.
- The existing kernel pts registry already correlates master/slave Spoors with
  a generation-stamped terminal. No new global terminal-name database is needed.
  Its header expressly states that no userspace-holdable Pts object exists today.

The source-derived ordering counterexample (not yet run as a guest regression)
is: broker records foreground A;
another session member calls SET_FG(B); A asks for clipboard; the graphical leaf
is still focused and the broker's old A record passes a graphical-only check.
An authenticated message saying that A used to own input does not close this.
Periodic GET_FG merely narrows the window. This is a design gap, not a claim that
the currently unexposed clipboard service leaks data.

## Prior art and fit

Plan 9's rio provides a `snarf` file for a shared text buffer. That filesystem
shape fits our existing per-session listener, but it does not establish the
stronger per-foreground-process rule we approved. [Plan 9 rio(4)](https://9p.io/magic/man2html/4/rio).

POSIX job control permits a controlling-session process to call tcsetpgrp,
subject to job-control rules; background SIGTTOU handling does not make the
shell the unique writer. Treating the shell as the kernel's exclusive authority
would silently narrow the existing terminal contract. [POSIX tcsetpgrp](https://pubs.opengroup.org/onlinepubs/9699919799/functions/tcsetpgrp.html).

Wayland ties selection delivery to keyboard focus and uses input serials to
relate requests to input. This supports binding clipboard admission to the
actual display owner; it does not identify the process behind a shared terminal.
[Wayland protocol model](https://wayland.freedesktop.org/docs/book/Protocol.html).

Fuchsia's clipboard RFC combines registered ViewRefs with input-focus checks and
view lifetime. It explicitly acknowledges transferable ViewRefs and focus races.
The useful precedent is a lifetime-bound registration validated against the
actual focus authority, not a string or a shell assertion. Our stronger ordered
admission promise still needs its own proof.
[Fuchsia RFC-0179](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0179_basic_clipboard_service).

Genode routes clipboard writes through Report sessions and reads through ROM
sessions, then combines domain identity with Nitpicker focus in its policy. The
fit is keeping storage separate from trusted ownership and policy rather than
putting editor state into the compositor.
[Genode clipboard design](https://www.genode.org/documentation/release-notes/15.11).

## Recommended direction: kernel-backed terminal ownership

Extend the existing pts seam for observation/admission, retaining its terminal
identity and legacy job-control behavior. Do not add a second shell-maintained
foreground table or grant Tapestry a general PTY master just to inspect focus.

The implementation contract should have these properties:

1. A monotonic foreground epoch belongs to the kernel pts incarnation. Every
   successful ownership transition or invalidation updates it; no wrap/reuse.
   Returning to the same numeric group still invalidates an older binding.
2. A terminal's real master holder can bind a narrowly scoped interaction
   observer to the intended compositor process incarnation. That binding conveys
   observation/admission only, not terminal read/write, signal or SET_FG powers.
   It dies with either endpoint or the terminal generation. No ambient new cap.
3. The observer can validate the requesting application's authenticated process
   incarnation against that terminal's current foreground membership and epoch.
   Caller-supplied PID, route token or executable name is never evidence. A
   pipeline still needs one explicitly nominated participating controller; a
   nominated process must actually be a live member of the foreground group.
4. Tapestry performs the terminal validation while processing the single ordered
   graphical focus check, so both facts hold at the admission point. Halcyon
   prepares the operation first, then matches exactly one reply and publishes.
   Merely validating at Bind or polling later is insufficient.
5. Ownership changes have reliable bounded readiness notification. New modal
   controller input/admission remains unavailable until the matching owner epoch
   has been acknowledged. Legacy applications retain their ordinary PTY path and
   APP fallback; ported shells must not be forced through a fabricated native
   controller. The implementation must define the ordering of pending input,
   stopped jobs, nested shells and direct SET_FG before accepting any client.
6. Seal the terminal host before it first runs when it owns this authority.
   Do not inherit that seal into ordinary applications or introduce an assumption
   that every same-user process is isolated from every other. No controller
   authority travels in terminal output, inherited environment strings or a
   broadly inherited pipe endpoint.
7. Reuse bounded pts slots and existing process/connection identity machinery.
   Enumerate the exact operations, error results, structures, registry mirrors,
   lock order and teardown paths before implementation. No callbacks or waits
   under the pts spinlock. In particular, the current pts -> process-table lock
   prohibition cannot be bypassed for a convenient lookup.

This deliberately expands the original "no new kernel IPC mechanism" scope.
It may be possible to extend existing syscall operations rather than allocate
new syscall numbers; either way it is a kernel ownership/ABI change. This review
requests approval of that scope, not approval of guessed numeric reservations.
The detailed ABI and lock/lifetime design would be recorded before its code,
coordinated with Main and Aux's current kernel work.

## Other directions

- **Defer native terminal integration.** Continue direct graphical clients and
  compositor-owned transcript interaction, while Nora/ut keep local registers
  and receive an honest shared-clipboard-unavailable response. This preserves
  the stronger boundary without expanding kernel scope now, but does not finish
  the requested Nora/ut workflow.
- **Trust cooperative shell notifications.** Less work, but direct SET_FG and
  lifetime races can leave stale controllers authorized. This weakens the
  approved security statement. Not recommended and not an implementation fallback.

A generic Mycelium migration would not resolve this: transport authentication
cannot attest state that only the kernel owns.

## Evidence required before enabling terminal clients

Test an old controller requesting after direct SET_FG from a different process;
A -> B -> A with unchanged numeric group identity; process exit/exec and group
membership changes; same-principal wrong terminal; observer/host death; pending
focus replies during handover; stopped/resumed and multi-process jobs; startup
and nested shells; trusted SAK takeover; all counter/slot exhaustion paths.
The denial must be witnessed by the affected operation, not inferred from a
prior boot test. Existing clean/mutant PTY/job-control models, affected kernel
regressions and the real Nora/ut workflow remain release requirements.
