# Native handle metadata query for Pouch spawn

> **On main** (branch b3, B-3b, operator vote 7 of 2026-10-14) this call is
> `SYS_HANDLE_FLAGS_GET = 129`: main's 128 is `SYS_ICACHE_SYNC_SPACE`, and Linux
> 129-131 are the kill family, which main translates (VIVARIUM 6.22). The text
> below keeps the numbering this design was written against on codex/astra.

Status: approved and implemented on October 11; bounded source-matched qualification passed.
Prepared and self-reviewed by Astra, October 11, against checkpoint 5d850a890.
The approved source extension and matching Pouch consumer are implemented.
The checks below qualify this boundary; full native Ninja support and production
packaging remain separate work.

## Problem and measured outcome

Pouch's static spawn interpreter can duplicate an invalid original descriptor
into a child slot and later discard that slot without rejecting the invalid
source. The final kernel descriptor list cannot validate a source absent from
that list. Its temporary-collision guard only covers a subset of these cases.

The interpreter also probes standard streams by allocating READ/WRITE dup
handles. When the parent handle table is full, those probes falsely treat
valid streams as absent. A second rights probe cannot distinguish absence,
insufficient rights and table exhaustion. File stat and poll are object
operations, not general handle-slot validity queries.

A 23-case host suite executes the actual proposed spawn implementation with a
mock native boundary. The pre-query implementation failed four new requirements:
discarded invalid source, overwritten invalid source, full parent table, and
failure to stop when the proposed query is unavailable. The proposal passes
all 23 cases at O0/O2 under LLVM 22 ASan+UBSan. Removing original-source queries
causes seven assertions to fail at both optimization levels. Four of these
assertions specifically enforce rejection before temporary opens; two detect
invalid sources later discarded and one checks the exact error.

## Approved native interface

Append `SYS_HANDLE_FLAGS_GET = 128` with one register argument, the caller's
handle index. Return 0 or `SYS_HANDLE_CLOSE_ON_EXEC` (bit 0), or a negative errno.
The bit reports the existing close-on-image-replacement metadata. This interface
adds no setter and does not change spawn or exec inheritance rules.

- `EBADF`: the full-width argument is outside the table or the slot is not live.
- `EINVAL`: no current thread/process exists at this internal entry point.
- No user-memory pointers, allocation, I/O, object reference acquisition,
  rights requirement, capability check, or cross-process target.
- The existing `handle_get_cloexec` takes the handle-table lock, checks the live
  slot and reads its flag within that lock. The wrapper checks the full 64-bit
  argument before narrowing to the handle index type.
- The result describes one instant. It neither pins the handle nor prevents a
  concurrent close/reuse. Spawn's existing caller-serialization requirement
  remains; this does not establish concurrent parent mutation support.
- C and Rust native wrappers mirror the number and flag. Pouch translates the
  native result; no Linux syscall ABI is added to Pouch.

Slot 128 overlaps Linux `restart_syscall`. Vivarium's existing mandatory
`VIV_ENOSYS` row consumes that Linux call before the native switch. Replace
only its former above-ceiling assertion with an explicit collision pin, and
retain a ceiling assertion at the next signal number, Linux kill 129. The
native ceiling becomes 128. No Linux row is forwarded into this new handler.
A runtime collision test remains mandatory before the interface is qualified.

## Pouch consumer

Before opening any temporary, query each DUP2 original-parent source with no
earlier child assignment. Return the query's errno on failure, even if later
actions close or overwrite the destination. Sources established by preceding
child OPEN/DUP2 actions continue through the existing ordered child model.

Seed standard streams using the same metadata query: EBADF means absent;
other errors stop the spawn. Historical native kernels return legacy -1 for
an unknown syscall; this query alone maps that value to ENOSYS because its
defined failures never include EPERM (-1). A real guest exposed the difference
from the initial mock. Explicit -ENOSYS is also passed through. Native wrappers
continue to return the raw kernel result. This removes
the allocating READ/WRITE probes. The close-history and temporary-origin guards
remain, as do destination/list limits and contiguous explicit inheritance.

A new libc using this path therefore requires the matching kernel. Do not ship
it with an older kernel or weaken ENOSYS into success. F_GETFD/F_SETFD, general
inheritance, tagged socket descriptors, process-group policy, signal masks,
group note delivery and atomic signal waits are not implemented by this step.
Ninja cannot be described as fully supported after this change alone.

## Pre-approval artifacts and checks (historical)

The reviewable patches are under
`work/oct11-native-tools/handle-query-proposal/`:

- `native-query.patch`: kernel interface/handler, Vivarium collision handling,
  native C/Rust wrappers. Applies cleanly to current source.
- `0050-pouch-spawn-source-query.patch`: proposed next musl patch, applies with
  fuzz 0 in a dry run to the existing 49-patch source. Not added to the series.
- `overlay/`: exact proposed files, separately retained from active source.
- `baseline/`, `proposal-final/`, `query-mutant-final/`: host seam logs.
- `handler-test.c`: actual proposed handler with mocked current-process and
  locked-lookup boundary. Eight cases cover valid flags, absent slots, full
  argument bounds including narrowing aliases, and missing process/thread.
  O0/O2 ASan+UBSan pass. This does not retest handle-table locking itself.

Both proposed kernel translation units pass AArch64 syntax checking with
actual headers. The native C wrapper cross-compiles with repeated header
inclusion. The proposed spawn translation unit cross-compiles with target
headers and a private header containing the proposed syscall constant.
The Rust wrapper is prepared but has not been compiled in this proposal.
No kernel was linked or booted; no native runtime or ABI compatibility gate
is claimed. Initial harness unused-function and missing include-path failures
are preserved alongside their corrected commands. The existing target header's
nested-comment warning remains visible.

## Integration checkpoints after approval

1. Apply the reviewed additive interface and Pouch patch, update the binding
   syscall/error references and native ABI mirror checks, and add regressions
   to the maintained host/native probes. Preserve the protected pending work.
2. Verify fresh patch application, boundary tests and native ABI/collision
   checks; build kernel and static/shared libc from recorded matching sources.
3. In an isolated ordinary-user guest, rerun the changed spawn witness with
   discarded/overwritten sources, exhausted-table metadata queries and narrow
   rights. Verify old-kernel failure and Linux 128 refusal separately.
4. Complete the applicable kernel gates, record single-agent self-review,
   source/artifact hashes and a clean checkpoint. Preserve the 8 GiB floor.
5. Continue the separate native descriptor-inheritance and process/signal
   contracts required for Ninja cancellation and console jobs. No debug-held
   spawn workaround, fake success, parked SQ work or private activation.

## Approval record

`CLAUDE.md`, Autonomy + escalation, explicitly reserves “Format breaks
(on-disk version bumps, wire-protocol ABI changes, syscall interface changes)”
for escalation. This is an additive native interface change, not merely a
Pouch bug fix. The operator explicitly approved adding this native metadata query and its
Pouch consumer, then executing the integration checkpoints autonomously on
October 11 after checkpoint267fffc32. That approval covers this implementation;
future descriptor/process ABI extensions remain separate design work.

## Integration evidence (October 11)

The native extension and patch0050 are now applied. The official50-patch
sysroot build compiles static/shared libc; native Rust wrapper cargo check
passes. The maintained suite expands to25 cases with historical -1 rejection
for both original-source and standard-stream paths. It executes the actual
series-produced source. The kernel handler harness extracts the active handler
and covers8 cases at O0/O2 under ASan+UBSan; native ABI mirror checks pass.

Runtime14 passed16 ordinary-user spawn cases,14 metadata checks including1019
extra handles saturating the table and transfer-only handles denying I/O,
and the Linux128 collision witness through a declared MPHENO_LINUX mount.
This run predates the legacy-error Pouch correction and does not qualify the
final libc. Kernel, libc, probes, daemon and fixture hashes are recorded under
`work/oct11-native-tools/`; the final probes reproduce byte-for-byte.

Preserved failures are explicit: runtime8 omitted the symbol regeneration
step after bare CMake and failed the existing stack probe; runtime10 launched
the Linux witness from a native mount; runtime11/13 used an already-open 9P
file as mount source, whose clone walk is refused. O_PATH supplies the required
walkable anchor. Runtime12 disproved the old-kernel ENOSYS assumption: raw -1
is its actual unknown-call result. None of these failed runs is a passing gate.
Scratch disks from8/10/11/13 were retired only after hashing, preserving source,
initrds, logs and reconstruction inputs. The 8448MiB runner floor stays active.

The final source-matched matrix passes all50 boots: ten each on default1/4/8
CPUs and UBSan4/8 CPUs. Every raw serial log records1830/1830 tests, stack-probe
PASS and boot OK, with no extinction or sanitizer failure. Both kernels have
matching generated symbol tables. The343-input snapshot matches committed HEAD
except the four approved kernel changes; the operator's pending test edits are
preserved outside this snapshot. The matrix uses its recorded historical
userspace fixture, not a newly baked production image.

Final runtime15 uses the rebuilt50-patch static libc and new kernel as uid1000:
all16 spawn cases,15 metadata checks and7 Linux-boundary checks pass. Metadata
queries work with transfer-only rights and all1024 slots occupied (1019 extra
duplicates); every duplicate close succeeds. A separately declared Linux mount
runs an actual Linux ELF whose syscall128 returns ENOSYS. Runtime16 uses the
old kernel and same rebuilt libc: the raw native unknown-call result is -1,
which Pouch reports as ENOSYS before child publication, leaving the output pid
unchanged. Both guests finish probe/shell/sync successfully and both complete
harnesses exit0 after QMP quit/serial EOF, with servers reaped and tokens removed.

The final25-case host suites pass at O0/O2 under ASan+UBSan; removing close,
source-query or legacy-error guards causes4/7/2 intended failures respectively.
The8-case handler suite also passes at both optimization levels. Final source,
libc, probe, kernel and fixture hashes are checked in
`work/oct11-native-tools/handle-query/final-verification.json`; raw logs and
failed-run dispositions remain beside it. All ten protected files match their
original hashes; external Stratum remains clean7dd103d. This is single-agent
self-review, not an independent audit. No Main landing, production bake,
dynamic-loader qualification, private activation or full Ninja qualification
is implied.
