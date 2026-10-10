# Parked SQ reuse subchunk — October 10

Operator asked to park the classifier-affected subchunk for Corona and continue
independent work. This records the immediately preceding SQ reuse correction as
that subchunk. The operator reported a security-classifier block; no rejection
payload or reason was visible in this turn. Do not infer a technical test failure
or attempt a bypass. Future Corona availability does not itself clear a restriction.

Patch: `sq-reuse.patch`, base `0fd8f8427a2e85536b07ecf6d384f2b3f1036a9e`.
Full modified files: `patched-source/`; hashes: `parked-manifest.json`.
**Unapplied** in the active checkout. Only these four agent-authored changes were
restored after byte-identical patch and source snapshots were preserved.
Built kernels under build/ include the patch: they belong to this run, not the
restored checkout. Do not use them as evidence for current source or rerun old
installers. Each native row retains source-matched cloned kernel artifacts.

Finding: loom_drain_sq released shared sq_head before reading its index and
copying SQE. A legitimate producer may immediately reuse index/SQE after head
acquire; the kernel lock excludes consumers, not that producer. Proposed fix
copies/rejects first, then releases head. Test-only hook simulates immediate
reuse for a valid SQE and a bad original index. It never activates private ENTER.
Single-agent self-review: preserve l->lock claim exclusion and CQ admission;
copy precedes permission to reuse, later chain routing uses only kernel copy.
Hook is KERNEL_TESTS-only under lock and touches shared memory only. No independent
audit or real userspace concurrent-scheduling proof is claimed.

Already-launched jobs finished before parking; no running jobs remain.
- Native smoke-20261010T065006Z: default1/4/8 and UBSan4/8, one boot each,
  1862/1862 tests, SQ regression, D7 overlap and20login cycles per row, min9944MiB.
  Ten raw logs, recorded source/diff, source/input/artifact hashes verified before
  restoring source. Five smoke rows, not a full50boot qualification.
- Host host-20261010T065053Z: exact consume/copy/release block passes sanitizer and
  optimized profiles; moving release back before copy reproduces cookie corruption
  and fails the assertion. Three source hashes/six commands verified.
- Astra pending/protected10hashes and cleanexternal61dde37 verified by native verifier.

Astra may continue independent fixed-I/O ownership and cancellation/retirement
cores with simulated backends. Native SQ publisher/integration, affected production
qualification and complete async release remain gated on resolving this dependency.
Retain async-first order; no jump to MM0-MM4, Main landing, private activation or
128MiB protection removal. Corona must review the patch and restriction context
before any future application; the operator may clarify if a different subchunk
was meant. Heartbeat instructions carry this explicit exception to no-peer-wait.
