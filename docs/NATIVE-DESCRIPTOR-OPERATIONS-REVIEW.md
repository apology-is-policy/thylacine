# Native descriptor operations: single-agent review

> **On main** (branch b3, B-3b): the calls are native 129/130/131 and the Pouch
> patches 0052-0055; main's Linux 129-131 collision guard is the kill family's
> TIER2 rows, not the ENOSYS rows reviewed below, so the Linux-collision
> witnesses this review cites (tools/test-descriptor-collisions.py,
> tools/host-tests/linux-descriptor-collision.S) stay on codex/astra. The
> evidence below is Astra's, from her branch, under her numbering.

Astra, October11. Approved design checkpoint925810047. This records self-review,
not an independent audit. The descriptor checkpoint is implemented and qualified
within the source, fixture and model limits below. It is not a production rollout.

## Ownership and ordering

The new native129 setter uses the existing table lock to check LIVE and replace
one close-on-image-replacement bit. It does not touch the object or its rights.
The new native130 target operation calls `handle_dup_to_checked`, extracted
from the existing replacement helper without changing its publication order.
The legacy wrapper still collapses checked errors to -1 for its existing callers.

Bounds and flag bits are checked in full-width registers before narrowing.
Absent source and prohibited alias are distinguished while holding the same
lock as acquisition and publication. A close/reuse before lock acquisition may
change which object the descriptor names; the operation never promises a pin
across separate calls. After acquisition, another close/reuse cannot invalidate
the acquired destination reference. The outgoing occupant is captured before
replacement, and released only after unlocking. A concurrent allocator cannot
observe an empty target between close and installation. Replacing an alias of
the same object acquires before releasing, maintaining one reference per live
slot. Full-table replacement needs no spare slot or heap allocation.

Source rights are copied verbatim. The alias predicate is unchanged: hardware,
Srv/Loom and devsrv Spoors remain non-duplicable. The destination can be any
caller-owned slot, just as it can be closed by its owner. There is no new
cross-process selection or privilege gate. SET/GET are metadata operations;
READ/WRITE authority must not be required or acquired to inspect these flags.

Linux129/130 now have mandatory ENOSYS classification rows. Previously their
absence was safe only while both numbers exceeded the native ceiling. Explicit
number pins and the next ceiling assertion at131 accompany the extension.
The table never sends a Linux signal request to native descriptor mutation.

## Pouch semantics and named limits

Patch0052 adds public F_GETFD/F_SETFD and exact-target dup2/dup3. Getter consumes
no variadic argument; setter consumes an int and maps flags explicitly. The
three approved descriptor operations exclude EPERM, allowing their adapters to
translate historical unknown-call -1 to ENOSYS. Other native returns retain
their existing treatment. Tagged sockets are checked by their existing owner;
unsupported live socket operations fail ENOSYS without invoking native mutation.
Self-dup keeps its qualified validation and does not clear an existing flag.

The spawn interpreter stores child-local standard-stream flags, applies ordered
actions, then removes final close-on-exec slots. Spawn DUP2 actions clear the
target flag, including self-actions. Ordinary OPEN clears the model slot flag;
OPEN actions with O_CLOEXEC are rejected before any temporary opens. An interior
hole still fails EINVAL, rather than being compacted or filled. No parent flag
is changed to implement child behavior. Existing original-source checks and
cleanup remain. Callers must still serialize parent close/reuse and action edits.

This does not implement general inheritance or arbitrary sparse child tables.
It also does not repair raw internal SYS_fcntl consumers: socketpair/signalfd
fallbacks, fopen/fdopen/freopen flag/status adjustments, __fopen_rb_ca and
fchown/fchdir validity fallback still reach the unsupported raw selector. Public
fcntl alone must not be advertised as fixing those paths or atomic O_CLOEXEC
open. Exact-target native dup now supplies __dup3, but full freopen behavior
still depends on its other operations. Native RFORK105/EXECVE104 exist; Pouch's
old no-fork/exec comment was corrected, not used to invent another spawn ABI.
Ninja startup groups, masks, cancellation, waits and full self-build remain open.

## Evidence and failed-run disposition

Evidence lives under `work/oct11-native-tools/descriptor-ops/`.
Actual-source host suites at O0/O2 with LLVM22 ASan+UBSan currently pass:
24 descriptor adapter cases,32 spawn cases,24 mutation-handler cases,8 getter
regression cases and12 self-dup cases. Native replacement-body tests use a
pthread lock and bounded reference owners for20,000 concurrent source close/
reuse iterations, exact target identity, flags, rights, full-table operation,
last-reference and same-object release. Those mocks do not qualify real device
close hooks. Kernel tests extend the existing target and Vivarium cases without
altering the protected test registry.

Nine Pouch mutants separately remove flag translation, tagged-owner refusal,
legacy translation, final spawn flags, action flag clearing, OPEN preflight,
close history or source queries. Three ownership mutants alter rights, flags or
release lock order. Actual Vivarium table/translator tests delete kill or tkill
rows and must observe the wrong forwarding verdict. Native guest collision
checks remain necessary because table tests do not exercise dispatch.

The official52-patch static/shared sysroot build matches the tested Pouch
sources. Three guest probes link twice byte-identically. Native Rust target
check passes with two pre-existing warnings. The first sysroot attempt was
stopped by the8448MiB runner guard; its log/exit/stop reason are retained. Twelve
completed superseded/failed initrds were gzip-archived and their decompressed
SHA256 checked before removing originals. Receipts preserve exact artifact
identity. The resumed build succeeded without lowering the disk floor.

The original mixed-kind handles clean run was stopped before the disk floor;
it is incomplete, not PASS. Its input snapshot, raw progress and retired state
cache hashes are retained. A one-process configuration attempt failed the
model's explicit minimum-two-process assumption and is not evidence. The additional
partitioned configurations retain two processes, unchanged actions/invariants/
handle bounds, and separately exercise transferable, hardware and Srv kinds.
Hardware and Srv partitions completed cleanly. The transferable partition also
hit the disk guard after 4,254,551 distinct states; it is incomplete, not PASS.
Its raw log, inputs and retired state-cache hashes are preserved. Existing
SPEC-POLICY suspends full clean runs on this surface; the required buggy
configuration gates below passed. No smaller replacement is claimed as proof.
All eleven existing two-process buggy configurations retain their original
inputs and require the named composite Invariants counterexample. These bounded
checks do not constitute completion of the original mixed-kind clean run, nor
model the descriptor bitmap or native argument parser. Those obligations are
covered by the explicit ordering review and host/kernel/guest witnesses.

The source-matched matrix passed all50 boots: default1/4/8 and UBSan4/8 CPUs,
ten each. Every raw boot log contains1830/1830 tests, the stack-probe PASS and
boot OK, with no extinction or sanitizer diagnostic. The343-file kernel snapshot
retains the earlier qualified test registry; protected test.c/devproc edits
are excluded. This matrix uses its recorded historical userspace fixture.

Runtime19 separately links the official52-patch static libc and passes34
ordinary-user descriptor checks, including all1024 slots occupied, narrow
rights, actual image replacement consuming flags, child-local spawn semantics
and socket refusal. Its actual Linux-personality ELF calls128/129/130 with live
descriptors and verifies ENOSYS plus unchanged stdout/stderr flags; the seven
mount/launch/cleanup checks pass. Runtime20 runs the same probe against the
older native128 kernel and passes six refusal/state-preservation checks.
Both whole harnesses exit0, with successful probe/shell/sync status, QMP/serial
EOF, reaped servers and removed ephemeral tokens. Sync success is not a reboot
persistence qualification.

`descriptor-ops/verify-final.py` verifies the raw logs, source manifests,
compiled probes, both libc artifacts, all11 existing model counterexamples,
host and negative-control results, ten protected-file hashes and the clean
Stratum pin. The caller-local metadata remains an instantaneous observation,
not a cross-call lifetime guarantee.
No production bake, Main landing, private feature activation or independent audit
is claimed. External Stratum remains clean7dd103d; parked SQ work stays untouched.


Two stale source comments were corrected after all runtime checks: the signal
ceiling now names131, and the checked helper distinguishes pre-lock validation
from locked source/alias errors. Comment-stripped text, including newlines,
is identical. Both default and UBSan kernels were rebuilt and their runtime
`.bin` artifacts are byte-identical to those used in the50 boots and runtime19.
Original source and ELF/bin artifacts remain in qualified-source/ and
qualified-artifacts/; comment-equivalence.json records both old/new source
hashes and rebuilt artifacts. This does not extend qualification to another
implementation or to the protected pending test-registry edits.
