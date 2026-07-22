# Debugging Playbook — Elusive Bugs, and the AEGIS-Corruption Triplet Case Study

**Status: binding, mandatory reading.** When you hit an elusive bug — one that
resists explanation, reproduces inconsistently, looks like "memory corruption,"
spans the Thylacine<->Stratum boundary, or has already burned a prior session
or two — **read this document before you start theorizing.** It exists because
one bug class cost this project the better part of a year of intermittent
hunting (issues #710, #712, #714, the "H1" investigation) under the wrong name
("AEGIS-256 content-sensitive corruption"). It was never one bug: it was a
*stack* of independent causes that all wore the same `STM_EBADTAG` mask, peeled
one session at a time. Sections 1-6 document the read-back triplet. **But the
recurring stratumd-MOUNT failure that kept "re-opening" after every "RESOLVED"
was the sharpest lesson of all -- and it was not a corruption bug. Read the coda
(section 6.10) before you trust any of this; it is where the method earns its
keep.** The method is the transferable asset; the specific bugs are just the
worked examples.

We are building an OS and a filesystem from scratch. Bugs of this shape — a
stack of independent defects that mask each other, surfacing only at a layer
boundary nobody had exercised yet — are not rare here. They are the house style.
Expect more. This is how you kill them fast.

---

## 0. The trigger profile — when this document applies

Reach for this playbook when **two or more** of these hold:

- The symptom reads as "corruption" / "AEGIS failure" / "mallocng assert" /
  "random fault in a fresh process" — i.e. a wrong-bytes symptom with no
  obvious culprit.
- It reproduces only sometimes, or only in the full system but never in
  isolation, or only after a reboot / a specific sequence.
- A prior session marked it "RESOLVED" or "AUDITED CLEAN" and it came back (or
  was never really gone).
- It spans a layer boundary you don't fully own in one place (kernel <-> driver,
  Thylacine <-> Stratum, userspace <-> 9P server).
- Your best theory keeps getting contradicted by the next data point.

If you recognize this, **stop theorizing and go get ground truth.** The rest of
this document is how.

---

## 1. The case, in one paragraph

corvus persists a per-user keypair wrap (a 3752-byte AEGIS-256-wrapped blob, an
*extent* file) and an `identity.db` to a disk-backed Stratum filesystem, so user
identities survive reboot. On the first boot everything passed; on the second
boot corvus logged `identity load dropped a user (missing/corrupt wrap)` twice,
`identity.db loaded (0 users)`, and the bootstrap re-create failed. For ~a year
this was chased as "AEGIS-256 content-sensitive heap corruption." It was, in
fact, **three independent bugs**, each masking the next, all newly exposed
because corvus was the first code in the project's history to write *and read
back* a multi-chunk extent over the disk-backed FS.

---

## 2. The year-long false trail — what "elusive" looked like

The bug had been seen as: a fresh native process faulting at a kernel VA; an
AEGIS-256 MAC failure deep in a mount; a mallocng assertion; "content-sensitive"
corruption that changed with the RNG seed. Each sighting got its own
investigation and its own partial fix:

- It was sometimes a real *other* bug wearing the same mask. **#713 was a
  genuine eret-window IRQ race** (an IRQ in the `ELR_EL1`-set..`eret` window
  landed EL0 at a kernel VA) — correctly found and fixed — but it was *not* the
  storage corruption, even though both presented as "random corruption in a
  fresh Proc." Two distinct bugs sharing a symptom kept the picture muddy.
- The immediately-prior session fixed two *real* bugs (a SrvConn shared-deadline
  starvation; a bdev non-sector-aligned-length **rejection**) and declared the
  blocker "RESOLVED + AUDITED CLEAN" with a green end-to-end test. **The close
  was hollow:** the E2E exercised create/AUTH/WRAP/UNWRAP entirely from the
  *in-memory* user record. Nothing ever read an extent back from disk. The
  bdev's "fix" (round the partial sector up and **zero-pad**) had stopped the
  EINVAL it used to return — and then silently wrote corrupt bytes that no test
  read back.

**Lesson already visible here:** a passing test proves only what it exercises.
An invariant-bearing feature (here: *durable* persistence) must be tested
through the durable / cross-boundary path, not a proxy. See §6.7.

---

## 3. The investigation journal — the actual path, dead ends included

This is deliberately honest about the wrong turns. The wrong turns are the
teaching content: a future agent's bug will rhyme with one of these pivots.

1. **Reproduce, exactly.** `build.sh all` (fresh pool) -> `test.sh` (boot 1,
   creates two users, exit 0) -> `test.sh` again on the *same* pool (boot 2):
   `dropped a user` x2, `loaded (0 users)`, create fails. A clean, deterministic
   repro is step zero; without it you are guessing.

2. **Read the binding design as a lead.** `CORVUS-DESIGN §16.6` showed an
   asymmetry: the `identity.db` path ended with a directory `fsync`
   ("dirent-durability barrier"); the keypair-wrap path did not. Hypothesis A:
   **dirent durability**. Added the missing `fsync` of the per-user dir +
   `users/`. (This was a real fix — Bug 2 — but on its own it changed nothing
   visible. See the masking trap, §6.1.)

3. **Boot 2 still failed.** Don't conclude "the fix was wrong." Get ground
   truth on what's actually on disk.

4. **Host-mount the pool.** Drove the *host* `stratumd` + `stratum-fs` (posix
   backend) against the post-boot-1 `pool.img`: `identity.db` (94 B),
   `users/michael/hybrid.corvus` (3752 B), `users/susan/` — **all present.** So
   the data is durable. Hypothesis A reversed: **not durability.** The
   host-vs-VM differential here was itself a clue (§6.4).

5. **Host-*read* the wrap.** `stratum-fs read .../hybrid.corvus` -> stratumd
   exited `rc=-201` = `STM_EBADTAG` (AEAD verification failed). The on-disk
   bytes are cryptographically wrong -> a **WRITE** corruption, not durability,
   not read. (Earlier I had only `stat`'d the file, which hid this. Read the
   bytes, not just the metadata.)

6. **Bisect the layer.** The posix backend round-trips a 3752-byte extent
   write->sync->read perfectly (`cmp` MATCH). So the extent/AEAD layer is sound;
   the corruption is **`bdev_thylacine`-specific** (the ARM/virtio backend).

7. **Prove it's not reboot/durability at all.** Added a corvus *same-boot*
   self-check: write the wrap, immediately read it back. It failed **in the same
   boot**. So: not reboot, not durability, not flush timing — a pure
   write+read-back failure.

8. **Red herring #1 — "it's a page-boundary / >4096 bug."** A size probe
   (write+read files of 200/1000/3752/4096/8192 same-boot) showed 200/1000 OK,
   3752+ FAIL. The 4096 boundary *looked* causal. It was a proxy (see step 16).

9. **A "fix" that made it worse.** Tried chunking the bdev transfer at the page
   boundary. The 3752 case now *hung* instead of corrupting. Reverted. ("Both
   multi-page paths broken" was a misleading dead end — the hang was a
   *different* multi-request weakness, not the bug.)

10. **Rule things out with verified facts, not assumptions.** Physical
    contiguity (kernel `kobj_dma_create` = single-order `alloc_pages` ->
    contiguous), VA->PA mapping (`fault.c` maps `burrow->pa + page_offset` ->
    contiguous), cache (QEMU TCG models no dcache). All eliminated by reading
    the actual code / knowing the emulator, not by hand-waving.

11. **Find a channel that reaches you, then instrument the driver directly.**
    Key observability fact: **stratumd worker-thread `stderr` does NOT reach the
    Thylacine console; the main thread's does** (it's where the startup banner
    prints), and the **kernel `uart` always does.** So a self-test placed in
    `stm_bdev_open_thylacine` (main thread, at mount) can `fprintf` freely.

12. **Isolated bdev is CORRECT.** The main-thread self-test wrote+read
    4096/4128/8192/8224 — including the exact zero-padded extent sizes — and
    *all round-tripped fine.* Hypothesis "bdev corrupts >4096" was **wrong.**
    Worker-thread (pthread) variant: also fine. A 200-op warmup (past the #712
    "~113th" count): also fine. Not size, not thread, not ring-state.

13. **The masking insight (§6.2).** A `write(X)` immediately followed by
    `read(X)` at the same offset can return *stale-but-correct* data: the shared
    DMA buffer still holds what was just written, so a read that fails to refill
    the buffer still "passes." **My passing read tests were masked.** The real
    FS path has intervening I/O (fsync, metadata) that flushes the buffer, so
    its reads are genuinely cold.

14. **The decisive write experiment.** Prime a window to `0xBB`, write a 4128-B
    pattern over it, read the window back: bytes `[4128, 4608)` came back `00`,
    not `0xBB`. **Confirmed Bug 1 — the partial-tail-sector zero-pad clobbers
    adjacent on-disk bytes.** Fixed with read-modify-write. The probe now
    preserves `0xBB`.

15. **Verify the fix at its own layer — and keep going.** With the RMW fix, a
    host read of the fresh boot-1 pool returned **all three files correct** (no
    EBADTAG). The write is fixed. **But corvus still failed boot 2.** A
    masking-bug stack: fixing Bug 1 unmasked the next defect. (§6.1)

16. **Red herring #2 dissolves.** A corvus read-error trace printed
    `r=-1 got=2048`: the *first* 2048-byte read succeeded; the *second* (offset
    2048) failed. Re-derive the axis: the failing files were exactly those `>
    2048` bytes — i.e. needing a **second** `Tread`. "inline vs extent" and
    "page boundary" were both proxies for **single-read vs multi-read.**

17. **Kernel-side trace nails it.** A `uart` trace in the kernel's `dev9p_read`
    (corvus's read goes kernel-side here, so `uart` reaches the console):
    `off=2048 rc=22(neg)` = `-EINVAL`. stratumd rejects a read at offset 2048.

18. **Find Bug 3 in code.** `stm_sync_read_extent` requires a 4096-aligned
    offset (`off % STM_UB_SIZE != 0 -> EINVAL`). The *write* path hid this via a
    `fs_write_extent_aligned_locked` helper; the *read* path passed the raw file
    offset straight through. Never exposed before because every prior reader read
    whole-file at offset 0. Fixed with the symmetric `fs_read_extent_aligned_locked`.

19. **All three together** -> boot 1 creates+persists, boot 2 loads 2 users +
    AUTHs with the reloaded wrap. Verified clean (no instrumentation), full
    suites green both repos.

---

## 4. The three bugs (the answer)

| # | Where | Mechanism | Why it hid | Fix |
|---|---|---|---|---|
| 1 | Stratum `bdev_thylacine.c` `op_write` | A block device transfers whole sectors. For a non-sector-aligned extent (block-aligned plaintext + 32-B AEAD tag, e.g. 4128) it **zero-padded** the partial tail sector, destroying `[len, sector_end)` bytes an adjacent object occupies. | Nothing read an extent back; an isolated same-offset write+read is masked; the clobber only bites when a *neighbour* shares the tail sector. **This is the real "AEGIS corruption."** | Read-modify-write the partial tail sector (mirrors the existing write-coalescing RMW). Stratum `91ae5d8`. |
| 2 | Thylacine `corvus` `persist_keypair_wrap` | `fsync`'d the wrap *file* but not the containing dirs, so the name->inode links weren't durable across a non-clean unmount — bytes on disk, path lost. | Masked by Bug 1 (until the write was correct, the read failed anyway). | `fsync` the per-user dir + `users/` dir. `CORVUS-DESIGN §16.6` reconciled. Thylacine `573b984`. |
| 2′ | *(A-1b audit-R1 refinement)* | Stratum's `Tfsync` is currently a **whole-pool** commit (`stm_fs_commit`), so the file-fsync already commits the dirents. | — | Bug 2 was therefore **forward-portable insurance, NOT load-bearing on this Stratum** — the load-bearing pair was Bugs **1 + 3**. The dir-fsync is kept for a future per-fid `Tfsync` (the POSIX FS contract). The lesson stands: **distrust your model of a dependency's semantics — verify it** (here, that `fsync(file)` does not commit the dirent — true on POSIX, not on whole-pool-commit Stratum). |
| 3 | Stratum `fs.c` `fs_read_regular_locked` | `stm_sync_read_extent` requires a 4096-aligned offset; the read path passed the raw file offset, so the 2nd chunked `Tread` (offset 2048) returned EINVAL. | Every prior reader read whole-file at offset 0 (host tools + all prior in-VM reads were `< one chunk`). Masked by Bugs 1 & 2. | `fs_read_extent_aligned_locked` (read-side counterpart to the existing write helper). Stratum `91ae5d8`. |

---

## 5. Why it was so hard — the structural traps

- **A masking stack.** Three independent bugs in series. Any one fix in
  isolation produced "no visible progress," which reads as "wrong fix" and
  invites abandoning a correct change. (You fixed durability — still fails on
  the write. You fixed the write — still fails on the read.)
- **A symptom shared with an unrelated bug.** "Random corruption in a fresh
  Proc" was *also* the #713 eret-window race. Two bugs, one mask, one of them
  already fixed — maximally confusing.
- **A hollow prior "clean close."** The E2E never exercised the durable read
  path, so "AUDITED CLEAN" was true-of-the-test and false-of-the-feature.
- **Observability boundaries that hid the failure point.** The corrupting code
  ran on a stratumd worker thread whose `stderr` is dead to the console.
- **Self-tests that masked the read bug** via shared-buffer staleness.
- **Correlations that were proxies** (size -> inline/extent -> single/multi-read).

---

## 6. The playbook — generalizable, in priority order

### 6.1 Suspect a masking-bug stack; verify each fix at its OWN layer.
When a plausible fix yields "no visible progress," do **not** assume it was
wrong. Confirm it independently *at its own layer* (here: host-read the pool to
prove the WRITE is now byte-correct, separate from whether the end-to-end test
passes), then keep going. Elusive bugs are disproportionately *stacks*.

### 6.2 Ground truth beats theory — always, and early.
Theory kept this bug alive for a year; one trace line (`off=2048 rc=22`) ended
it. The instant a theory is contradicted twice, stop reasoning and **instrument
to observe the actual state at the failure point.** Print the bytes, the
offsets, the return codes, the on-disk content. The cheapest decisive experiment
beats the most elegant deduction.

### 6.3 Know your observability boundaries before designing the experiment.
You can only conclude from a channel that reaches you. In this system:
- **Kernel `uart_puts`/`uart_putdec`** always reaches the console — trace
  syscall handlers (`dev9p_read`, etc.) freely.
- **stratumd / pouch main-thread `stderr`** reaches the console (the startup
  banner proves it); **worker-thread `stderr` does not.** Put a driver
  self-test in the *main-thread* open/mount path, or relay worker state to the
  main thread, or encode it into a return code the kernel surfaces as an
  `Rlerror` ecode.
- **Native procs (corvus, joey) `t_putstr`** reach the console.
Find the channel first; then write a test that uses it.

### 6.4 Use the host-vs-VM (or any backend) differential as a probe AND a clue.
Stratum's posix backend round-trips what `bdev_thylacine` corrupts -> the bug is
backend-specific. And *why* the host couldn't reproduce it (it read whole-file
at offset 0; the VM read in chunks from offset 2048) was itself Bug 3's
signature. When two implementations of the same contract disagree, the
**difference in how they exercise it** is the lead.

### 6.5 Make cold tests genuinely cold; beware shared-state masking.
A `write(X)` then `read(X)` against a shared buffer (DMA region, cache, page) can
return stale-but-correct data and mask a broken read. Force the cold path: an
intervening operation that overwrites the shared state, or read data you did not
just write, or read after a reboot.

### 6.6 Chase the mechanism, not the correlation.
"Fails above 4096 bytes" was true and useless — size was a proxy twice over.
When a fix doesn't move the needle the way your correlation predicts, the
correlation is a proxy; **re-derive the causal axis.** (Here: re-derive
size -> inline-vs-extent -> single-Tread-vs-multi-Tread.)

### 6.7 A passing test proves only what it exercises.
Before trusting "RESOLVED/CLEAN," ask: *does the test traverse the real,
durable, cross-boundary path, or a proxy?* For an invariant-bearing feature,
exercise the invariant directly (read the persisted bytes back across a reboot;
read at a non-zero offset; cross the layer). Then wire that test into the suite
so the regression can never go silent again.

### 6.8 Cross-layer bugs need single-surface ownership.
This triplet spanned Thylacine (corvus, kernel `dev9p_read`) and Stratum (bdev,
fs). Finding Bug 3 required treating the boundary as one engineering surface and
following the read down into Stratum. If a bug crosses a repo boundary, get the
authority to fix it on both sides (see `CLAUDE.md` "Stratum coordination").

### 6.9 Encode the hard-won invariants where they can't rot.
Two domain invariants this bug taught, now load-bearing:
- **A block backend that cannot write sub-sector MUST read-modify-write the
  partial tail** (zero-padding clobbers neighbours), and its read counterpart
  must align the offset down. Any new block backend repeats this or repeats the
  bug.
- **`fsync(file)` makes data durable; the dirent (name->inode link) is not
  durable until you `fsync` the containing directory.** Every durable-create
  path needs the directory barrier.

---

## 6.10 Coda — the fourth cause: the recorded reproducer lied (resolved 2026-05-30)

The triplet in sections 4-6 was real and is fixed. But the cross-reboot kept
**re-opening with the identical `STM_EBADTAG`, now at stratumd's pool MOUNT** --
*before* corvus or any read-back even runs. A 3rd session cornered it to "a
stratumd-pattern corruption, silent to mallocng, between the byte-perfect read
and the AEAD decrypt of a 128 KiB btree node," built two heap-torture binaries
that both came back clean, preserved a failing pool at
`build/fixtures/REPRO-ebadtag-201/`, and handed off "NOT fixed; instrument the
read->decrypt window."

A 4th session did exactly that -- and the answer was **not corruption at all.**

**What the instrument found.** A heap-free probe (stack `snprintf` + `write(2)`,
zero `malloc`, so it cannot perturb the allocator it observes) at the single
decrypt chokepoint `stm_btree_node_decrypt`, run on BOTH the host and the VM
against the same pool, showed **every node decoding byte-identical to the host
reference across 23 boots** -- same ciphertext FNV, same tag bytes, same derived
key. There was no corruption anywhere in the read->decrypt window. The VM that
"failed deterministically" passed 23/23.

**The reproducer was a lie.** `REPRO-ebadtag-201/` was a *mismatched pair*: a
`failing-boot.log` stamped 18:05 (a pre-fix, mid-debugging build) bundled with a
`pool.img` stamped 18:32 -- baked *after* the day's fix commits landed at 18:21.
The pool+key were a matched, working set; the log was from a different epoch. A
"deterministic reproducer" that passes N/N is not a reproducer.

**The actual root cause.** `tools/build.sh`'s `pool` target re-bakes
`build/fixtures/system.key` (libsodium-random, fresh per run) but did **not**
rebuild the ramfs that bakes that key in at `/system.key`. So any caller that
re-baked the pool via `build.sh pool` -- notably `tools/test-cross-reboot.sh`
itself -- left the ramfs holding a **stale** key. The VM then mounted the *fresh*
pool with the *wrong* key; stratumd derived the wrong metadata key; AEAD tag
verification *correctly* rejected the first btree node -> `STM_EBADTAG` ->
`run failed (rc=-201)` at mount. The "content-sensitive intermittency" was
build-command HISTORY: it failed after `build.sh pool` and passed after
`build.sh kernel`/`all` (which rebuild pool and ramfs together). Confirmed both
directions -- `hash(fixtures key) != hash(ramfs key)` after `build.sh pool`,
boot the mismatch -> `rc=-201`, rebuild the ramfs to match -> boot OK. Fix:
`tools/build.sh` `b7066e4` couples `build_ramfs` to the `pool` target;
`test-cross-reboot.sh` then passes 3/3.

**The generalizable lessons -- the sharpest in this document:**

1. **The build/test harness is part of the system under test.** A "year-long
   content-sensitive memory corruption" was a shell-script ordering bug. When the
   failure is at a key/crypto boundary, suspect the *key pipeline* -- provisioning,
   propagation, staleness, who-rebuilds-what -- with the same rigor as the data
   path. Hash the key at every hop; a one-line `shasum A != shasum B` ended a year.

2. **A `MAC`/`AEAD` failure means wrong bytes OR wrong key/nonce -- never assume
   "corruption."** The tell was visible the whole time: the Merkle check (over the
   ciphertext) *passed*, then AEAD *failed*. Correct ciphertext + failing tag is
   the signature of a *parameter* mismatch, not data damage. Split the two with an
   instrument before you theorize about heap footers.

3. **A preserved reproducer can lie -- verify provenance and re-derive from
   scratch.** Re-confirm it still reproduces, check that its artifacts are from
   one epoch (the log and the pool were 27 minutes and three fix-commits apart),
   and prefer a *recipe* you can re-run over a frozen blob. The frozen blob had
   already been overtaken by a fix when it was saved.

4. **One mask, a stack of causes.** `STM_EBADTAG` wore *four* distinct root causes
   across this year: op_write tail-clobber, the read-offset EINVAL, the #713
   eret-window IRQ race, and finally this stale-key footgun. Every "RESOLVED" was
   true for the cause it found and false for the next. **When the ghost returns,
   it is a new bug wearing the old face -- re-derive from ground truth, every
   time.** This is exactly the masking-bug-stack discipline of section 6.1, and it
   does not stop just because a prior session sounded confident.

---

## 6.11 The flake-dismissal anti-pattern (the most expensive habit)

Every bug in this playbook was first dismissed as something cheaper. "AEGIS
corruption" was "intermittent / content-sensitive" for ~a year. #713 was "rare,
IRQ-timing-dependent." The EBADTAG was "intermittent." The pattern is not a
coincidence -- it is the rule: **the nasty bugs are intermittent BY NATURE,
because they are races, resource-margin cliffs, and ordering bugs. "Flaky" is the
SYMPTOM of that class, never the explanation.** The deterministic bugs are the
easy ones; you fix them and move on. The ones that cost months are the ones you
were tempted to wave away.

"Flaky" / "transient" / "boot variance" / "timing-dependent" / "just KASLR" /
"just QEMU scheduling" / "works on re-run" is a **hypothesis with the same burden
of proof as any other root cause** -- a definitive, reproducible mechanism. It is
also the most *convenient* hypothesis (it lets you proceed without digging), which
is precisely what makes it the most dangerous. **The convenience is the tell.**

The discipline (now the `elusive-bug-hunt` skill's earliest trigger):
1. The instant you reach for a flake word, STOP. Do not silently absorb it. Never
   write it into a commit / status / memory as a CONCLUSION -- the only honest
   unproven label is **"UNEXPLAINED -- suspected X, NOT confirmed; deep-dive
   queued."**
2. **Quick pre-analysis (minutes):** name the convenient hypothesis AND the one
   cheap observation that confirms or refutes it. The convenient theory is often
   already weak on inspection -- check the *mechanism*, not the vibe.
3. **Surface to the user** with the pre-analysis + what a definitive proof needs.
   The user queues the deep dive at the earliest convenient point (park the chunk
   now if it could be impacted; else right after it ships) -- tracked, never
   dropped.

Two worked examples, caught the moment they were waved away (2026-05-30; both
queued for deep dive, not yet root-caused -- this section will be updated when
they are):
- **"rfork_stress_1000 boot-flaky kernel-stack-overflow."** The convenient label
  was "KASLR stack placement." Pre-analysis REFUTED it in one read: `kaslr.c`
  randomizes only `__stack_chk_guard` (the canary cookie) and the kernel *image
  base* -- NOT the per-thread kernel *stack* allocation (`thread.c`: a fixed 16
  KiB usable + 16 KiB guard). A fixed stack cannot overflow "because of KASLR."
  What actually varies run-to-run is **interrupt timing**: an IRQ taken at the
  deepest point of the rfork/exec/wait call chain pushes an exception frame onto
  the same 16 KiB stack and can cross the guard. Leading hypothesis: the kernel
  stack margin is **too thin for worst-case call-depth + interrupt nesting** -- a
  real resource-margin bug, intermittent because IRQ arrival is. Definitive proof:
  paint the stack with a sentinel, run the stress, scan the high-water mark, and
  measure the margin to the guard (and/or mask IRQs across the test and see if the
  overflow stops).
- **"Normal QEMU TCG boot variance" (boot straddles the 45s timeout).** The
  retry count barely moves (616 vs 621 -> ~5 ms), so the tens-of-seconds swing is
  NOT joey's 1 ms-paced `/srv`-bind poll. Unchecked confound: the timeout runs
  happened while a concurrent build + a heavy Opus audit agent loaded the host;
  the passing re-runs were on an idle host. Decisive experiment: run `test.sh`
  N times on an IDLE host -- if consistently < 45 s, it is host CPU contention
  (benign; the fix is don't-run-concurrent or bump the timeout); if it STILL
  swings, instrument the boot phase timeline and look for a **bimodal / cliff**
  signature (a race or a missed-wakeup falling back to a deadline -- the I-9
  class). Either way "normal" is unproven until measured.

### 6.11.1 The ownership-dismissal sibling ("not mine" / "pre-existing")

The flake dismissal says *"this isn't a real bug."* Its sibling says *"this is a
real bug, but not my problem."* Same convenience, different face -- and the
`elusive-bug-hunt` skill now trips on BOTH families. The disownment vocabulary:
**"not mine", "not my chunk", "pre-existing", "already broken", "unrelated",
"out of scope", "someone else's subsystem", "in-flight elsewhere", "known bug",
"known flake", "tracked already", "v1.x", "deferred."** The instant one lands on
a *live, reproducing* defect, STOP -- it routes to CLAUDE.md "Whole-system
stewardship" (the system is OURS), not to a close.

**Attribution is not ownership.** Proving a defect is pre-existing/cross-tree is
GOOD triage and IS the ground-truth method (6.2): stash your change, rebuild on
the base, reproduce -- if it fails there too you've isolated it from your work.
Do that. But the finding changes only WHO introduced it / WHERE it lives, never
whose job it is (ours) or how urgent it is. The failure is using a correct
"pre-existing" proof to disown, deprioritize, or close a chunk around the bug --
worse than skipping the investigation, because it launders a dodge as diligence.
The honest label is **"pre-existing + OWNED, queued as <task>"**, never
"pre-existing, not mine" with a period. And the FIRST disposition is to enqueue
it (a task + a memory note), the moment it's confirmed real, before deciding
fix-now vs sequence-later -- prose in a commit body is not ownership.

Worked failure (2026-06-04): during #826b-2 the full Stratum ctest surfaced a
reproducing `STM_ECORRUPT` in `test_compound_ops_concurrent` (concurrent reflink).
The instance correctly proved it pre-existing -- stashed the chunk, rebuilt on the
`304dbd1` base, reproduced 3/3 -- then wrote "in-flight Stratum bug, not mine" and
moved on. The proof was right; the conclusion was the breach. Correct move: same
proof, then own it + enqueue it (task #842). (It was also mislabeled "transient
flake" when it currently fails deterministically -- a 6.11 violation stacked on
the ownership-dismissal one.)

---

## 6.12 The two 6.11 flakes, resolved (worked example, 2026-05-30)

The two symptoms 6.11 caught being dismissed were deep-dived FIRST the next
session. **Both were real; neither was a flake.** The hunt is a clean worked
example of 6.11's protocol + three reusable lessons.

**#789 boot variance — proven HOST-side (benign), not "normal QEMU".** Built a
phase-timestamping harness (`build/flake789/harness.py`) and ran the idle-host
distribution: BIMODAL with a clean ~7 s cliff (19-26 s | 33-37 s). Per 6.11 the
bimodal-vs-smooth question is decisive, so the cliff alone said "real, not
jitter." Localized it: the kernel phase (boot + 629 tests + irq-bench) is
CONSTANT ~11 s (1-CPU-bound); the entire swing is the post-suite userspace
phase — the only part that spreads work across all 4 vCPUs. Decisive
discriminators: `-smp 1` (one vCPU thread) → 0.39 s spread; `taskpolicy -b`
(force the throttled/E-core tier) → 170-220 s; default `-smp 4` → bimodal. Root
cause: **macOS placing QEMU's 4 TCG vCPU threads across the Apple-Silicon M2's
4 P-cores vs 4 E-cores** — an E-core in the mix drags the synchronized guest
~2.5x. Host-scheduling artifact, not a guest bug. (Bonus: the committed
BOOT_TIMEOUT comment blaming "stratumd worker pthreads" was wrong; the real
constant long pole is the irq-bench kernel-side busy-spin.)

**#788 rfork_stress kernel-stack-overflow — real bug, but the obvious fix was
REFUTED.** Reproduced 3/15 on a fully idle host (`EXTINCTION: kernel stack
overflow` inside `proc.rfork_stress_1000`). "KASLR" doubly refuted (kstack is
buddy-allocated). Measured: boot stack uses 4.5 KiB (ruled out); deepest
surviving per-thread kstack ~6 KiB; no deep-growth code path exists (`exits()`
shallow; IRQ-nesting CODE-refuted — `sched()` runs IRQ-masked). The obvious fix
is "bump the 16 KiB stack." The **forcing experiment killed it**: shrink the
kstack 16→8 KiB; if it were depth, an 8 KiB stack would overflow MORE (a chain
reaching >16 must pass through 8). Result: 8 KiB → **0/15** overflows. Smaller
stack → FEWER overflows REFUTES depth. Combined with extreme timing-fragility
(fires only on the exact clean build, 3/15; ANY source change → 0/~100), the
"stack overflow" is a SYMPTOM: a corrupted SP/pointer landing in a guard page
under a narrow SMP timing race — same CLASS as #713. (Hindsight caveat: the
8 KiB result ALONE is confounded — shrinking the stack also recompiles, so it
cannot by itself refute depth; the un-confounded probe is next.)

**RESOLVED (same session, by audit + a NON-recompiling probe — no GDB needed).**
The decisive move was the `-smp` discriminator: run the BYTE-IDENTICAL clean
binary at `-smp 4/2/1` — vary only a QEMU flag, never the binary, so the timing
window survives. Result: stack-overflow **2/20 (smp4), 1/20 (smp2), 0/20
(smp1)** — it NEEDS a secondary CPU. In parallel a focused soundness prosecutor
(Opus subagent) found the exact race: **`thread_free()` gates on thread STATE
but not `on_cpu`**, so `test_smp_work_stealing_smoke` frees a worker that is
`SLEEPING` but still `on_cpu==true` — its own `sched()` is mid-
`cpu_switch_context` on a secondary that the host descheduled onto an E-core
(the #789 substrate is *why* the window is wide enough to lose the race). The
freed SLUB slot + order-3 kstack are buddy-LIFO-recycled by `rfork_stress` ~4
tests later (the "work_stealing_smoke precedes the overflow" fingerprint in all
5 logs); the stalled secondary's late register-save corrupts the recycled
thread's ctx → wild SP → guard fault. A **use-after-free**, not depth, not an
IRQ-stack issue. **Fix = an `on_cpu` gate in `thread_free`** (mirror of the
audited `wait_pid` reap spin); bumping the stack would have masked it. The
R5-H F76 fix had closed only the RUNNING flavor of this same "free a thread mid-
cpu_switch_context" race; this was the SLEEPING/EXITING-but-on_cpu flavor.
Verification footgun: the fix can't be confirmed by "rebuild → overflow gone"
(the rebuild detunes the window regardless) — confirmation is the root-cause
triple-lock (code + the all-5-logs fingerprint + smp1→0) + correctness by
construction (the gate is a no-op when `on_cpu` is false, the canonical wait
when true), with a counter that flags the window if it is ever hit at runtime.

**Four reusable lessons:**
1. **Vary the RUNTIME, not the binary.** The probe that broke #788 open changed
   only a QEMU flag (`-smp 4/2/1`) on the byte-identical clean binary — so the
   timing window survived (unlike every recompile). smp4 2/20 → smp1 0/20 proved
   "needs a secondary CPU," which collapsed the search to the SMP scheduler and
   let a focused audit name the exact `thread_free`/`on_cpu` race. When a bug is
   too fragile to instrument, the FIRST move is a non-recompiling runtime knob
   (cpu count, accel, memory, clock) that can still shift the *category* of cause
   — it resolved #788 without ever needing to observe the fault directly.
2. **Instrumentation detunes a timing race.** Adding probes (or even changing a
   constant → recompile) shifts code layout and closes a narrow window: 0/~100
   instrumented vs 3/15 clean. When a bug vanishes the instant you instrument,
   that fragility IS the finding (it is timing, not depth/size). To OBSERVE such
   a bug you need a tool that doesn't change the binary (GDB hardware watchpoint).
3. **A "size/depth" symptom that gets RARER as you shrink the resource is not a
   size bug.** Let the forcing experiment run the wrong direction — it falsifies
   the easy fix cheaply, before you commit a masking patch. (Caveat: a shrink is
   also a recompile, so on a timing race read it alongside a non-recompiling
   probe, not alone.)
4. **Verify the measurement instrument, not just the target.** A probe here
   reported a fully-UNUSED stack as fully-USED (high-water cursor init'd to the
   low end, not the high end). Caught only by SYMBOLIZING the alarming
   "deepest tid=5" — it was a thread created+freed without ever running. The
   "beware self-tests that mask the bug" rule (6.5) applies to probes too.

---

## 6.13 #806 cracked — build the diagnostic, then seed-pin the repro (2026-05-31)

The other half of the "rfork_stress kernel stack overflow" symptom (the part #788's
on_cpu fix did NOT explain) was root-caused and fixed this session. It is the
sharpest worked example yet of three transferable moves.

**The bug (one line):** `directmap_walk_to_l2/_l3` (the kstack-guard demote in
`arch/arm64/mmu.c`) do a break-before-make on the direct map — transiently
unmapping a 1 GiB / 2 MiB block across a `tlbi+dsb_ish` — WITHOUT masking IRQs. A
timer IRQ in that window runs a handler that dereferences `current_thread()` (a
slab object reached via its direct-map KVA); when that thread's PA is in the
demoted block, the read faults, and the fault handler re-derefs the same wild
pointer → the recursive fault that descends the boot stack into its guard. Fixed
by masking IRQs across the BBM.

**The moves that cracked a year-long bug:**

1. **Build the diagnostic that makes the bug HONEST before hunting the root.**
   The symptom (`boot-stack guard` overflow) was a *lie* told by an amplifier: a
   wild `current_thread` made the fault handler re-fault, recursing one frame per
   fault until the boot stack crossed its guard. HX-1 (the crash dump) + the #806
   re-entrancy guard (`g_in_kernel_fault`, extinct on the *second* kernel-fault
   entry with the nested FAR) converted the lie into the truth: `recursive kernel
   fault ... 0x<wild current_thread>`. **You cannot root-cause a symptom that is
   itself a downstream artifact — first invest in the instrument that surfaces the
   real first fault.** (This is why the amplifier fix, though it did not fix the
   root, was the prerequisite for fixing it.)

2. **For a layout-sensitive bug, pin the layout knob to FORCE a deterministic
   repro — vary the RUNTIME, not the binary.** The repro was ~1/24 and "any
   rebuild detunes it to 0" — because QEMU randomizes `/chosen/kaslr-seed` per
   boot, and the KASLR slide moves the kernel's *physical* base, which shifts
   where the buddy zone (hence every Thread/kstack) begins. So the SEED selects
   the heap layout. Feeding a fixed DTB via `-dtb` pins the seed (verified: same
   dtb → identical KASLR offset twice). A `dtc`-edited seed search over ~24
   pinned seeds on the guard-equipped binary re-opened the window on seed 10 and
   caught the honest dump. Same lesson as 6.12's `-smp` discriminator: when a bug
   is too fragile to instrument, find the *non-recompiling runtime knob* that
   controls it (here the DTB seed) and pin it. (Caveat: it was still probabilistic
   on the pinned seed — the layout is necessary, the IRQ-timing coincidence also
   needed — which itself CONFIRMED the timing leg.)

3. **A prosecutor reasoning from the AMPLIFIED dump mis-rates the true root.**
   Three independent prosecutors ran. One actually FOUND this exact code
   (`mmu_set_no_access_range` BBM not IRQ-masked) but rated it **LOW-fit /
   "almost certainly NOT #806"** — because it reasoned from the old boot-20 dump
   whose terminal fault was the *boot-stack guard* (a TTBR1 access), and concluded
   the bug couldn't be in the direct-map regime. The honest dump (a *direct-map*
   L2 fault on `current_thread`) flipped that LOW to the root. **When the captured
   dump is a downstream artifact, every severity judgment built on it is suspect;
   re-rate against ground truth from the honest instrument, not the amplified
   symptom.** Corollary to 6.1's masking-stack rule: an amplifier is a kind of
   mask, and it mis-weights the prosecution, not just the diagnosis.

**Verification (per the timing-bug discipline, not rebuild-and-rerun):** the
recompile detunes the seed→repro map, so a clean A/B is impossible. Confirmation
is the root-cause triple-lock (honest-dump backtrace + ESR DFSC=0x06 L2-read +
the wild direct-map `current_thread` + the unguarded BBM in code) plus
correctness-by-construction (IRQs masked across the BBM → no handler can observe
the transient unmap → the fault is structurally impossible on the boot CPU), with
the #806 re-entrancy guard as the permanent runtime tripwire for any recurrence.

---

## 6.14 #810 cracked — gdbstub a HANG; distrust the inherited hypothesis (2026-06-01)

The "exit_group cascade SMP race" — an intermittent boot HANG (no EXTINCTION) at
the `pouch-hello-exitgroup` smoke, ~1/2 at `-smp 4`, worse under host load — was
handed off across sessions with a *named root-cause hypothesis*: a missed
death-wake / on_cpu spin in `proc_group_terminate`'s cascade reap. It was wrong.
The real bug: **secondary CPUs had no per-CPU timer, so a CPU-bound EL0 thread on
a secondary was never preempted** — the exitgroup test's `main()` busy-spun
`while(g_started<2)` on a secondary, monopolizing it while its worker sat RUNNABLE
in that CPU's run tree, never scheduled; `g_started` never reached 2, `exit_group`
never ran, joey's `wait_pid` hung. The cascade never even executed. Fixed by
arming the per-CPU timer on secondaries (deferred to the production transition so
the UP-like in-kernel tests stay quiescent). Five transferable moves:

1. **A HANG is a gift: capture it with a non-perturbing gdbstub.** Unlike a crash
   (which needs HX-1 to fire), a hang sits perfectly still. Boot QEMU with `-s`
   (gdbstub, running — `run-vm.sh` passes it through), detect the stall in the
   UART log (reached marker X, no marker Y, log size stable, QEMU alive), then
   attach `lldb` in batch: `gdb-remote :1234` + `target modules load --file
   thylacine.elf --slide <KASLR-offset-from-the-banner>` gives a symbolized
   all-vCPU backtrace + `print` of any kernel global. This is the timing-race
   analog of 6.12-lesson-2 ("instrumentation detunes"): the gdbstub observes
   without recompiling. The capture harness (`build/hunt810/gdbcatch.sh`) loops
   until it catches a hang (~1/2), so even an intermittent bug is caught in 1-3
   boots.

2. **The inherited hypothesis is a hypothesis, not a fact — re-derive from ground
   truth.** The handoff's "cascade reap race" framing had survived multiple
   sessions unverified. The gdbstub disassembly of the surviving EL0 thread —
   `yield; ldar w9,[x8]; cmp w9,#0x2; b.lt` — is uniquely `main`'s `g_started<2`
   loop, not a worker and not `proc_group_terminate`. I *also* misread it on the
   first (register-only) dump as `worker_spin`; the *second* dump (with
   disassembly) corrected me. Read the actual instructions, not the symbol you
   expect. (§6.2, applied to an inherited theory.)

3. **The `-smp` discriminator collapses the search.** `-smp 1`: 10/10 boots clean.
   That single fact proved the hang NEEDS a secondary CPU, ruling out every
   non-SMP cause and pointing straight at "what's different about secondaries" —
   which led to the per-CPU-timer asymmetry. (Same move as 6.12 for #788; vary the
   runtime, not the binary.) The `g_ipi_resched_count[]` global (read via lldb)
   ruled out an IPI-delivery gap in the same pass.

4. **Grep the history before re-treading it.** A `git grep`/doc sweep surfaced
   `docs/handoffs/011-p2db-eb.md`: per-CPU-timer-on-secondaries had been tried
   TWICE (P2-Dd) and reverted, because it exposed two latent SMP races. Both were
   since fixed by *other* work (#788's `on_cpu` gate; the EL1h conversion / I-21).
   So the fix was sound *now* even though it had failed before — and the two
   historical failure tests (`rfork_stress_1000`, `preemption_smoke`) became the
   precise regression targets to re-prove. Whole-system stewardship pays here: the
   answer was in the tree's own memory.

5. **A correct fix can EXPOSE a latent bug — that's not a regression in your fix,
   it's the masking stack in reverse (§6.1).** Arming the secondary timer at
   bring-up immediately crashed `scheduler.preemption_smoke`
   (`thread_free of RUNNING thread`): the in-kernel tests are deliberately UP-like
   (`sched_set_notify_enabled(false)` parks secondaries), and a per-CPU timer gave
   each secondary an independent wake → it stole a test thread. The fix wasn't
   wrong; it unmasked an assumption. Resolution: defer the arm past the test phase
   (mirror the existing notify gate). The matrix (smp4 20-boot 0/20 hangs + 678/678,
   UBSan 5/5, smp8 8/8) is the empirical gate for a timing fix that can't be A/B'd
   by rebuild-and-rerun; an independent adversarial prosecutor + a self-audit
   converged CLEAN, with the load-bearing argument being correctness-by-construction
   (every CPU now has the preemptive tick + syscall/fault paths are non-preemptible).

---

## 6.15 The recurrent class — the multi-thread-Proc lift outran per-Proc shared-state serialization (RW-2 + RW-4, 2026-06-10)

The single most productive question in the HOLOTYPE re-review has been: **"this
per-Proc structure was written when a Proc was single-threaded — who serializes
it now that threads are real?"** P6 (pouch-threads) made a Proc a set of peer
Threads sharing one address space, one handle table, one Territory, one set of
service connections — and `rfork(RFNAMEG)` shares a Territory *across* Procs.
Every structure that was implicitly protected by "only one thread of control ever
touches my Proc's state" lost that protection the moment threads landed, but the
locks/multi-waiters did not all arrive with them. The defects are **latent**
(no current in-tree workload drives two threads into the same structure
concurrently) yet **EL0-reachable** (a user pthread program — or two RFNAMEG
Procs — reaches them with no privilege), so they read as P1: correct under
current coverage, a kernel UAF/extinction under a realistic-but-absent workload.

Independently surfaced instances (two RWs, four findings, one root cause):

- **RW-2 2C-F1 [P1]** — a poll registration outlives the polled object's ref: a
  peer thread closing the fd frees the object while another thread's poll waiter
  still points into it (UAF).
- **RW-2 2B-F1/F2 [P1]** — `wait_pid`'s single-waiter `child_done` + a lockless
  cond walk: two threads reaping concurrently corrupt the walk / lose a wake.
- **RW-4 SA-F1 [P1]** — the per-Territory mount table + `root_spoor` carry no
  lock at all (only `dot_lock`, added by LS-4 for the cwd string); a concurrent
  `pivot_root`/`unmount` on one thread frees a Spoor a walking thread is mid-read
  on (`root_spoor` UAF), and `mount_lookup` returns a source a concurrent
  `unmount` just freed.
- **RW-4 R2-F1 [P1]** — the byte-mode `/srv` connection's single-waiter `Rendez`
  was safe only under the 9P-client lock serialization (one drainer per ring);
  the P6-pouch-sockets byte-mode userspace `read()` path bypasses that lock, so
  two threads reading one conn fd trip `extinction("rendez already has a waiter")`.

The already-fixed precedents are the same class caught earlier, one structure at
a time: **#844** (handle-table lock), **LS-4** (`dot_lock` for the cwd string),
**#713** (`vma_lock` for the VMA list — "stratumd is the first heavily-threaded
Proc"), **#847** (per-Burrow dual-refcount). Each was found in isolation; the
RW-review made the *pattern* legible.

**The generalizable move — when you touch ANY per-Proc-shared structure:**
1. Enumerate its writers and readers. If more than one Thread of a Proc (or more
   than one RFNAMEG-sharing Proc) can reach it concurrently, it needs a lock or a
   multi-waiter — "the current programs don't do that" is the latent-P1 trap, not
   a safety argument (the kernel must be sound against any EL0 program).
2. A **single-waiter `Rendez`** on per-Proc-reachable state is a red flag: it is
   safe ONLY if something *else* guarantees a single drainer (e.g. a client lock).
   The instant a second code path reaches the same wait without that guarantee,
   the single-waiter assertion is an unprivileged extinction. Prefer the
   `poll_waiter_list` multi-waiter, or an explicit single-reader busy-guard (the
   `devcons` pattern: a 2nd concurrent reader gets `-1`, never a 2nd waiter).
3. A lock added for *one* field of a shared struct (LS-4's `dot_lock` guards only
   `dot_path`) does NOT cover the rest of the struct — the unguarded siblings
   (`mounts[]`, `root_spoor`) are the next finding. Audit the *whole* struct.
4. The fix is mechanical (a near-leaf lock, taken around the read-modify-write,
   never held across a blocking call — the `dot_lock` discipline: copy/swap under
   the lock, free/clone/walk outside it). The hard part is *finding* every member;
   the cross-cut (RW-10) maintains the ledger of per-Proc-shared structures ×
   their serialization so the sweep is exhaustive, not one-finding-at-a-time.

This is the soundness twin of the masking-bug-stack (§6.1) and the flake/ownership
dismissal (§6.11): the convenience-seeking read is "no program does that, so it's
dormant." It is not dormant — it is *latent*, and latent-reachable-from-EL0 is a
live soundness defect. Enqueue + fix or escalate; never file it "tracked dormant."

---

## 6.16 #359 cracked -- QMP live-corpse autopsy + the preemptible-holder spinlock class (2026-07-04)

The parallel on-device `go build` wedged the whole guest syscall-silent
(~1-in-1.5 boots). No extinction, no output -- the hardest observability case:
a LIVE corpse. Three method entries earned here:

1. **QMP autopsy works on a wedged guest (even under HVF).** `info registers
   -a` over the QMP socket dumps every vCPU's PC/PSTATE/GPRs while the guest
   spins. PC - KASLR offset -> `nm` names the spin site; PSTATE's DAIF bits
   say who is maskable. The scratch toolkit (`qmp_dump.py`, `pc_resolve.py`,
   `walk_threads.py` -- a proc-tree thread enumerator that fp-walks RUNNABLE
   threads' saved stacks via `ctx.fp`/`ctx.lr`) turned "syscall-silent wedge"
   into a full holder stack in one capture. The decisive frame: the ONLY
   RUNNABLE thread held the contended lock, preempted mid-critical-section --
   every spinning CPU was IRQ-masked, so nothing could ever run it again.

2. **The class: a plain spinlock held by a PREEMPTIBLE context, contended by
   IRQ-masked spinners.** Thylacine syscalls/faults run IRQ-masked end-to-end;
   kthreads and fresh-thread spawn thunks run IRQ-enabled. Any plain lock
   shared between the two tiers could deadlock exactly this way -- `c->lock`
   was merely the first the go build exposed. The fix is the rule, not the
   instance: plain `spin_lock` now disables preemption per-thread
   (`Thread.preempt_count`; ARCH 8.11), `sched()` asserts no lock is held
   across it, and an unbalanced release extincts at its own site.

3. **The fix's OWN first cut had a classic bug the diagnostics caught: a
   per-CPU count torn by preempt+migrate mid-increment.** The `ldr/add/str`
   RMW computed the slot address first; an IRQ mid-RMW read the pre-increment
   value (0), the gate passed, the thread migrated, and the `str` landed in
   the OLD CPU's slot -- poisoning it non-preemptible forever (a livelock with
   the kernel still breathing: frozen stats counters but live periodic dumps
   was the signature) while the unlock underflowed the NEW CPU's slot. TWO
   simultaneous extinctions, char-interleaved on the UART, were the two
   halves of one migration event. Lessons: (a) per-CPU data mutated by
   preemptible code is the Linux `this_cpu_*` problem -- Thylacine's answer
   is per-THREAD state that travels with the migration; (b) when two CPUs
   extinct concurrently the UART interleaves char-by-char -- de-interleave
   by extracting hex runs and resolving CANDIDATE ranges, or serialize the
   extinction path first; (c) an underflow probe (`dec at count==0 ->
   extinction at the release site`) converts a silent poisoned-gate livelock
   into a named one-line diagnosis. The probe caught BOTH the first-cut
   per-CPU tear AND the raw/counted asymmetry on sched's early-return -- it
   pays rent.

Full record: `memory/bug_359_9p_client_deadlock.md` + the #360 sections in
`docs/reference/15-scheduler.md` and ARCH 8.11.

## 6.17 The HVF idle-~256% storm -- a leaked busy-yielding debug fixture (2026-07-22)

User-reported: "HVF sits at 300% CPU at idle; used to be ~5%." A performance
regression is a real defect (never "host load"), and this one hunted cleanly by
the method:

1. **Ground-truth the magnitude AND the mechanism first.** measure-idle
   (headless HVF, settle, sample qemu %cpu) reproduced ~256%. But %cpu is a
   host-side proxy -- the *guest* signal is `/ctl/sched`'s `parks=` delta over a
   quiet window (an interactive login that read it twice): **~17,000 parks/sec**
   at idle, `runnable: 1`. So NOT a never-parking loop -- a ~17 kHz park/wake
   ping-pong (the CPUs park, then are woken almost immediately). This
   distinguished "a thread that never sleeps" from "a thread woken constantly,"
   which pointed at a spinning *userspace* process, not a kernel idle bug. Note:
   the `wake-ipi` counter is really "woke before the 4 ms backstop one-shot" --
   it includes device IRQs, so its 99% dominance did NOT mean IPIs specifically
   (an easy false lead).

2. **The compositor-toggle differential exonerated the prime suspect in one
   experiment.** The gfx/aurora arc was the obvious guess. `THYLACINE_NO_GPU=1`
   (joey SKIPs the compositor -- `/srv/tapestry absent`) still idled at 248% ~=
   255%. The storm was non-compositor; the differential saved a wrong deep-dive.

3. **git-bisect on the %cpu, pruned by "what runs at the login prompt."** The
   regression window was ~50 first-parent commits, but the storm was present at
   the bare login prompt where the editor (nora/kaua/parley -- most of the
   window) does not run, so that whole range was excludable a priori. Per-commit
   build+measure walked 5% -> 5% (PTY exonerated) -> 27% -> 29% -> **256% at
   c585fc7f** ("bake Ambush"). The jump was exactly where `/ambush` got baked,
   which flipped the `ambush-probe` boot test's launch stages from SKIP to RUN.

4. **The class: a leaked busy-yielding debug fixture.** `ambush-probe` stages C
   (`ambush exec`) and D (`dap-selftest`) LAUNCH `/ambush-child`, a Go debuggee
   whose `parkLoop` was `for { runtime.Gosched() }` (a tight yield). The debugger
   exits on stdin EOF, and per I-39 NoStrand a debugger-*launched* target is
   RESUMED (not killed) on debugger death -> orphaned to init, busy-yielding
   forever. ambush-probe held no handle to those debugger-spawned children (the
   "killgrp" its comment promised was never implemented). ~2-3 leaked yielders
   pegged ~2.5 cores. Fix: park the fixture (`runtime.Gosched()` + a short
   `time.Sleep`) so a leaked instance idles at ~0% -- the busy-yield was obsolete
   once 8c-2 made a *parked* target debuggable. Lessons: (a) a debug/test fixture
   that can outlive its harness must be INERT when leaked, never a busy-loop;
   (b) "debugger death resumes the target" (NoStrand) is correct for an ATTACHED
   target but leaks a LAUNCHED one -- the launched-vs-attached lifetime asymmetry
   is a standing trap; (c) an **idle gate** now guards the class:
   `tools/ci-idle-gate.sh` (`make idle-gate`) fails if idle qemu %cpu exceeds a
   no-core-pegged threshold. It is host-load-robust by construction -- host
   contention only *deflates* qemu's %cpu share, so it can never false-FAIL an
   idle guest, while a real spin (>3x the threshold) fails decisively.

Full record: `memory/bug_hvf_idle_300_regression.md`.

## 7. Appendix — reproduction recipe

```sh
# Fresh pool, two-boot cross-reboot repro (the canonical elusive-persistence test):
tools/build.sh all          # re-bakes a fresh pool.img
tools/test.sh               # boot 1: creates state
tools/test.sh               # boot 2 on the SAME pool: loads it back

# Host-mount the pool to inspect what actually persisted (ground truth):
SD=build/host-stratum/src/cmd/stratumd/stratumd
SF=build/host-stratum/src/cmd/stratum-fs/stratum-fs
"$SD" build/fixtures/pool.img --listen /tmp/x.sock \
      --keyfile build/fixtures/system.key --root-dataset 1 --backlog 4 &
"$SF" -s /tmp/x.sock ls   /var/lib/corvus/users          # dirents present?
"$SF" -s /tmp/x.sock read /var/lib/corvus/users/<u>/hybrid.corvus | wc -c  # bytes readable? (EBADTAG => write corrupt)
```

**Key-staleness footgun (section 6.10):** never re-bake the pool with a bare
`tools/build.sh pool` and then boot without rebuilding the ramfs -- the ramfs
bakes `/system.key` in, so a stale ramfs key against a fresh pool is a
`STM_EBADTAG` at mount that looks *exactly* like corruption. As of `b7066e4` the
`pool` target rebuilds the ramfs for you; `build.sh kernel`/`all` always did. A
fast sanity check when a mount EBADTAGs: `shasum build/fixtures/system.key
build/ramfs-src/system.key` -- if they differ, it is the key, not the bytes.

Observability cheat-sheet: kernel = `uart_puts`/`uart_putdec`; stratumd
main-thread `stderr` reaches console AND the boot log via joey's fd-2 pipe
(the mount path is main-thread, so its diagnostics surface); native procs =
`t_putstr`.

---

*Written 2026-05-29 after the A-1b read-back close; coda (section 6.10) added
2026-05-30 after the recurring mount-EBADTAG was root-caused as a build-harness
stale-key footgun -- NOT corruption. The bugs are real; so is the method. Full
root-cause detail lives in the session memory
`bug_large_9p_write_srvconn_runtime.md`; the fixes are Stratum `91ae5d8` +
Thylacine `573b984` (the read-back triplet) + Thylacine `b7066e4` (the stale-key
harness fix).*
