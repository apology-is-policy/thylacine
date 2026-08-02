---
id: chg-2026-08-02-devices-content-sweep
type: chg
title: "vault sweep: the three Devs that own their bytes -- and a public constant behind the fail-closed gate"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-content
  - lock-random
  - lock-rng-dev
  - inv-i1
  - inv-i16
  - inv-i28
  - inv-i32
  - inv-i33
  - moc-kernel-devices
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 22, the FOURTH and final sweep of `devices/` -- **the area is complete**.
Read from code: `kernel/devramfs.c` (662), `kernel/env.c` (452),
`kernel/devenv.c` (376), `kernel/random.c` (654), `kernel/chacha20.c` (102) plus
three headers. Main had not moved since batch 21 (`631c8ade`), so the branch was
already synced; L-1 checked for the TENTH time and still absent, so address-space
stays deferred.

THE BATCH'S FINDING: **three Devs that own their bytes, and therefore face three
questions the rest of the area does not.** Everything else in `devices/` presents
something that exists independently -- a device's registers, a process's state, a
server's files. These three hold the only copy, which moves the hard problem off
MEDIATION (batch 21's projection) and onto IDENTITY, LIFETIME and COPYING. All
three answer all three, and their answers are opposites because each is forced by
what the content is:

- the boot filesystem's content arrives from outside and CAN NEVER LEAVE -- the
  table holds pointers INTO the cpio blob, so the long-intended free is blocked
  by the shape of the structure that reads it, not by anyone's priorities;
  identity is positional (index + 1);
- the environment's content is the Proc's own, and its identity must be
  MANUFACTURED -- a monotonic never-reused id, PLUS a per-Env device number,
  because ids restart at 1 in every Proc so two unrelated first variables both
  present as "file 1";
- the entropy source's content is manufactured on demand and its LIFETIME RUNS
  BACKWARDS -- a served byte is zeroed as it is copied out and every rekey
  destroys the key that produced everything served so far. It has no identity at
  all (walk always misses). Where the other two must persist, this one must have
  already ceased to exist.

**F1 -- THE FIRST KEYSTREAM BUFFER IS A COMPILE-TIME CONSTANT.** `rng_rekey_locked`
generates its buffer from the CURRENT cipher state, and on the first call that
state is BSS-zero -- a public value. The seed is XORed only into bytes [0,40),
which become the new key and are then erased; the remaining 984 -- exactly the
window `g_rng_have` marks servable -- are the UNKEYED keystream, identical on
every boot of every machine. What prevents them reaching a caller is that
`random_seed_from_virtio` (main.c:568) always runs a second rekey before any
consumer, and its cheap-collection rekey is UNCONDITIONAL (it runs whether or not
the virtio pull succeeded). **The exposure is INVERTED**: on an RNDR-less target
the readiness gate is still shut in that window so a read fails safe, while on an
RNDR-present target `devrandom_init` opens the gate and a read there returns the
constant. VERIFIED: the first rekey runs from the zeroed state; the seed reaches
only [0,40); the servable window is [40,1024); the second rekey is unconditional
and precedes every consumer (`exec.c:342` AT_RANDOM, `devdev.c:433`,
`syscall.c:5603` -- all post-boot). NOT VERIFIED as reachable: no consumer exists
in that window, so this is structural, not live. **No test can see it** -- every
random test runs in the post-boot test phase, by which time the buffer is secret.
The reference doc records the ORDERING, but for the seeding property (a
KASLR-correlated key), which is a different property with the same guard.

**F2 (task #31) -- TWO DOCUMENTS SAY SEEK-TO-END WORKS ON AN /env FILE. IT DOES
NOT.** `dev->seekable` gates SYS_LSEEK (`syscall.c:1597`) and pread/pwrite
(1120/1179); devenv contains ZERO occurrences of `seekable`, so the flag is
false and the refusal fires BEFORE the SEEK_END arm (1615) ever reaches
`spoor_stat_native`. And the flag exists precisely for this: RW-4 R2-F2 replaced
a `stat_native == NULL` heuristic that "regressed lseek to succeed on an offset
their read/write ignore", and `dev.h` + the handler both carry comments saying so.
**The guard did exactly its job -- and devenv.c:176 and 128-devenv.md both record
the belief the guard exists to falsify.** `env_read`/`env_write` do honour
offsets correctly, so the flag looks simply forgotten. Dormant (nothing seeks
/env today); the fix appears to be one line, for a main-track session.

**F3 -- THE ROT RULE'S THIRD FORM: a fact restated twice is corrected once.**
`RAMFS_FILE_MAX` went 32 -> 64 -> 128 -> 256; the comment block EXPLAINING the
raises is meticulous (it names which binaries the 64 cap silently dropped), and a
second comment 30 lines below still says 128. `34-devramfs.md` says 32, says
"~270 lines" (662), and contradicts ITSELF on its test count -- 10 in one section,
15 in another, 24 registered. The edit is always scoped to the thing being
CHANGED, never to the fact being changed, and this happens identically in code
comments and in prose.

**F4 -- AND ITS FOURTH: a document derived from a comment inherits the comment's
error.** F2 is in BOTH devenv.c and 128-devenv.md, because the doc was written
from the comment. Two independent-LOOKING sources now assert it. Added to the
area's reading rule: **two sources agreeing are one source if one was written
from the other.**

SMALLER FINDINGS. The cpio mode fallback's justification has EXPIRED -- it says
"v1.0 mkcpio.py always emits 0100644", but #58 made the generator preserve each
source file's real permissions so binaries carry their execute bit (I nearly
built a false finding on top of the stale claim -- the fallback is still cold,
only its stated reason is void). An INHERITED /env fd reports its PARENT's devno:
the stamp lands at walk time from the walking Proc, and a child re-resolves
contents against its own Env while keeping the stamp -- the one case the stamp
cannot see (not reachable from the Image cache it protects, which always
re-walks). `devenv`'s perm_enforced comment claims a flip "would fail-close every
walk-open via devenv_stat's -1" -- it would NOT: `stalk.c:591` consults
`spoor_stat_native`, which devenv has and which reports the CALLER as owner, so
every check would pass and the flip would be silently INERT. Right decision,
non-existent guard rail -- and a comment promising noise where the reality is
silence discourages the test that would find it. Conversely `dev_register`
carries a real cross-field boot invariant (`wstat_native` without
`perm_enforced` -> extinction), which is the constructive opposite.

`env_linux_encodable` is the batch's best-argued piece of code: the flat
`/proc/<pid>/environ` render SKIPS entries it cannot encode (a '=' in a name, a
NUL in a value) rather than emitting them raw, because raw would not TRUNCATE the
answer but CORRUPT it -- and "absence is a state every getenv caller already
handles" where a wrong value it cannot distrust is not. Notably the same render
goes to real trouble to be offset-aware precisely so it never drops a variable
silently, so it ACCEPTS for the unencodable case exactly the failure mode it built
machinery to avoid, because there the alternative is worse.

THE BOOT FILESYSTEM MANUFACTURES DIRECTORIES THAT CONTAIN NOTHING -- six of them,
existing only to be mounted over, because a mount needs a path that resolves and
the boot root is a read-only archive on which no mount point can be created. They
are world-searchable so I-28's per-component X-gate passes, and that execute bit
is the only thing its permissions actually say. Third distinct SENTINEL style in
the area (separation by MAGNITUDE, after a reserved value and a high bit).

DEVRAMFS SURFACES ITS TRUNCATION; DEVPCI (batch 21) DOES NOT -- and devramfs's own
comments record the cap silently truncating TWICE, once dropping files the boot
expected. It learned because it was bitten, which is the area's rule stated from
the other direction.

MEASURED. 40 registered tests across the five files. No locks in devramfs (built
pre-SMP, read-only after -- immutability again); [[lock-env]] already existed from
the introspection sweep (the environ render), so it was linked rather than
rewritten; [[lock-random]] and [[lock-rng-dev]] are new and DELIBERATELY never
held together -- the device pull allocates and spins entirely outside the state
lock so the allocator sits under the device lock and the state lock stays a pure
leaf. The device poll is bounded TWICE (wall-clock AND an unconditional iteration
ceiling) because each covers the other's blind spot: a fixed count expires early
when the guest spins natively while the host completes asynchronously, and a
real-time bound assumes a clock that advances.

INVARIANTS. No new invariant minted; five extended. I-16 gains the obligation the
RNG owes it (the slide is PUBLISHED, so anything else derived from the same
firmware seed must decorrelate -- and the stronger answer is declining to count it
toward readiness). I-1 gains the shape where isolation is undone by an IDENTITY
COLLISION rather than a missing check, and the lesson that correct per-caller
resolution is insufficient if the names handed out are not unique, because a
consumer that trusts names as identities is entitled to. I-32 gains the note that
subsystem-local bounds (the env's 64 x 4096) are the same invariant at smaller
scale and are NOT on its table, so the table reads more complete than it is. I-28
gains the synthetic-mount-point mechanism. I-33 gains the boot root's
seed-its-own-path-at-birth exception.

LEDGER. `34-devramfs.md` had a REFERENCE.md row (repointed); `106-random.md` and
`128-devenv.md` have none -- both among the 48 unindexed from 99 up, the gap seen
from the other side again.

PROBE. Four sabotages, each asserted ON DISK before linting (the batch-19 lesson),
restored via `cp` from scratchpad copies taken AFTER the last real edit (the
batch-21 lesson -- a backup predating an edit silently reverts it). Three caught
with DISTINCT messages: a dangling wikilink, a renamed required section, an
unknown id in a change record's `touched`. The fourth -- `kind: spun` on a lock
note -- passed, and checking the schema BEFORE calling it a linter gap showed it
is BY DESIGN: `kind` is documented `spin|spin-irqsave|leaf|...`, open-ended, and
the corpus uses it as free text (8 of 26 lock notes carry parenthetical
qualifiers). So a typo there is invisible and the field cannot be relied on for
grouping -- a property, not a defect. The linter DID catch a real error of mine
beforehand: `audit: heavy` is not in its enum, and `hard` was the right value
anyway, the CSPRNG being on the audit-trigger list.
