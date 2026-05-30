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
