---
id: sub-kernel-content
type: sub
parent: moc-kernel-devices
title: "Three Devs that own their bytes"
code:
  - kernel/devramfs.c
  - kernel/devenv.c
  - kernel/env.c
  - kernel/include/thylacine/env.h
  - kernel/random.c
  - kernel/chacha20.c
  - kernel/include/thylacine/random.h
  - kernel/include/thylacine/chacha20.h
audit: hard
guarded-by: [inv-i1, inv-i16, inv-i28, inv-i32, inv-i33]
validated-by: [prose, gate-smp]
locks: [lock-env, lock-random, lock-rng-dev]
abis: []
design:
  - "docs/ARCHITECTURE.md section 9.4"
  - "docs/ARCHITECTURE.md section 9.7"
  - "docs/PORTABILITY.md section 6"
created: 2026-08-02
updated: 2026-08-16
---
## Purpose

The boot filesystem, the per-process environment, and the random source. Three
Devs that do not present anything — they *are* what they serve.

## Contract

- **`/` (the boot ramfs)** — the files a cpio archive shipped in the initrd,
  flat, read-only, every one system-owned. It is the root the machine starts in,
  and the tree the first process is executed from.
- **`/env`** — a directory of the calling process's environment variables. Read
  a name for its value, write or create to set, remove to unset, enumerate for
  the names. The mount is global; the contents are never anyone else's.
- **`/dev/random`** — an endless stream of bytes nobody can predict, or an error
  if the kernel cannot yet promise that.

## Mechanism

**Everything else in this tree presents something that exists without it** — a
device's registers, a process's state, a server's files. These three hold the
only copy. That moves the hard questions off mediation and onto three others:
*what names this, how long does it live, and what happens when it is copied.*
Each answers all three, and their answers are opposites, because each answer is
forced by what the content is.

**The boot filesystem's content arrives from outside and can never leave.** A
bootloader places a cpio archive in memory; the kernel parses it once and builds
a table whose name and data fields are **pointers into that archive**. Nothing is
copied. The consequence is not incidental: the archive can never be freed, and
the long-standing intent to release it once the real filesystem mounts is blocked
by the shape of the table that reads it, not by anyone's priorities. Identity is
positional — a file's name in the protocol is its index in the table plus one.

**The environment's content is the process's own, and its identity has to be
manufactured.** A variable is named by a **monotonically increasing id**, assigned
at creation and never reused, so a handle to a variable removed between a walk and
a read fails cleanly instead of resolving to whatever now occupies its slot. But
the id alone is not enough, because ids restart at 1 in every process while the
mount is global — so two unrelated processes' first variables would both be
"file 1" and *claim to be the same file*. Each environment therefore also mints
a **device number**, and the pair names the file. This is not tidiness: the
executable-image cache is keyed on exactly that pair, so without it executing a
path under `/env` could serve one process the contents of another's variable.
**A cache in an unrelated subsystem is what makes the identity load-bearing.** A
forked child gets a fresh device number with its copied variables, because a copy
is not the same file.

**The random source's content is manufactured on demand, and its lifetime runs
backwards.** Where the other two must make their bytes persist, this one must
make them *stop existing*. A served byte is zeroed in the buffer as it is copied
out, so it can never be handed to a second caller. Every refill re-keys the
cipher **from its own fresh output** and then erases the bytes it keyed from — so
the key that produced everything served so far is gone, and an attacker who
captures the state cannot roll it backwards. It has no identity at all: a single
unnamed stream, whose walk always misses, because there is nothing here to name.

**The environment is the one that is written to, and writing is where its shape
shows.** A value is stored in an allocation sized exactly to its length, so any
write past the end reallocates and copies; a short write over a longer value
keeps the tail, which is ordinary file behaviour rather than a special case.

**The environment's flat-block reader gained a second caller, and the rule
guarding it had to be rewritten because the new caller correctly violates it.**

The block read — the whole environment serialized in one span — was written for
the introspection device, which resolves an arbitrary process under the table
lock and is therefore **cross-process**. The block reader itself checks no
identity, so the rule attached to it read: *do not add a second caller without
carrying the gate.*

The second caller is the exec path's projection of the environment onto a new
image's stack, and it carries **no gate** — correctly. It projects a process's
**own** environment onto its **own** new stack, reaching nothing that process
could not reach by reading the environment device itself. The gate exists for
the case where **reader and owner differ**, which is the introspection device's
case and not exec's.

**A rule stated as a mechanism is violated correctly by its first legitimate
exception.** "Carry the gate" names the remedy; the property is "reader and
owner may differ". Stated the first way, the exception looks like a breach and
the rule looks wrong — and the usual outcome is that the rule quietly stops
being cited.

The rewritten form demands an argument in either direction: a new caller is
same-process (no gate, and say why) or cross-process (carry the gate). **What is
forbidden is a caller that does neither** — which is a stronger obligation than
the original, not a relaxation of it.

**The boot filesystem manufactures directories that contain nothing.** A mount
needs somewhere to land — a graft onto a path that does not resolve is not a
mount — and the boot root is a read-only archive, so a mount point cannot be
created the ordinary way. The root therefore synthesizes six empty directories
whose only purpose is to be mounted over. They are the reason the machine can
assemble a namespace before it has a writable filesystem.

**Each of the three names its own things in its own space, and two of them hide a
sentinel inside it.** The ramfs separates its synthetic directories from real
files **by magnitude** — a base so far above any possible index that the two
ranges cannot meet — and the environment reserves zero as both the free-slot
marker and the root directory, which is consistent only because real ids start at
one. This is the third distinct sentinel style in the area, after a reserved
value and a high bit ([[sub-kernel-hwcap]], [[sub-kernel-discovery]]).

**The random source refuses to serve rather than serve weakly.** Reads fail until
an *unobserved* strong source has contributed. That word is the whole design:
the bootloader-supplied seed is real entropy, but the boot-time address
randomization derives the kernel's publicly visible load offset from **the same
seed**, so it is entropy someone can partially observe. It is mixed in as
material — through a deliberately different avalanche function, so the two
derivations do not correlate ([[inv-i16]]) — but it does not count toward
readiness. Only the CPU's own generator, or a pull from the host, flips the gate.

**That pull is bounded twice, and each bound covers the other's blind spot.** The
host device completes asynchronously on another thread while the guest may be
spinning at native speed, so a fixed spin count can expire before a perfectly
healthy answer arrives — the bound must be **real time**. But a real-time bound
assumes the clock advances, so an unconditional iteration ceiling sits underneath
it for the case where it does not. Neither alone terminates correctly in both
worlds.

**A pull that returns nothing but zeroes is rejected as a failure.** All-zero is
what a dead device or an incoherent transfer produces, and treating it as entropy
would be the worst possible outcome — so the source keeps its previous seed
instead. The retry budget differs by caller for the same reason: the boot pull
retries, because its failure leaves the whole system refusing to serve, while the
periodic top-up does not, because its failure falls back to a cheap re-key and
must never stall the caller that triggered it.

## Data structures

A fixed table of 256 ramfs entries, each a name pointer, a data pointer, a size
and a mode — all four pointing into the archive. A per-process environment holds
64 slots, each an inline name of at most 64 bytes and a separately allocated
value of at most 4096, plus the id counter and the minted device number. The
random state is a cipher context, a 1024-byte output buffer, a count of bytes
remaining in it, and a countdown to the next strong re-seed.

## Concurrency

The ramfs table is built before secondary processors start and is read-only
afterwards — immutability again, as in [[sub-kernel-discovery]]. The other two
lock: [[lock-env]] guards one environment (peer threads of one process share it,
so there is no single-writer story), [[lock-random]] guards the cipher state, and
[[lock-rng-dev]] serializes the hardware pull.

The two random locks are **deliberately never held together**: the device pull
allocates pages and spins on a completion, so it runs entirely outside the state
lock, and the absorb takes the state lock afterwards. The device lock therefore
sits above the page allocator while the state lock stays a leaf — which is only
acyclic while no interrupt handler takes either, and both current callers are
ordinary process context.

The environment's lazy allocation is a **compare-and-exchange**, because two peer
threads of one process can both find no environment and both allocate; the loser
frees its own and adopts the winner's.

## Invariants enforced

**[[inv-i1]]** — the environment is per-process content behind a global mount, so
every operation resolves the *calling* process, and no permission check is
involved: there is nothing reachable to deny. The minted device number is what
keeps that isolation from being undone by an identity collision in a cache
elsewhere.

**[[inv-i16]]** — the deliberate decorrelation of the random seed from the
address-randomization seed, and the refusal to count shared entropy toward
readiness, are this invariant's other half: the offset is disclosed, so anything
derived from the same source must not be.

**[[inv-i28]]** — the boot root's synthetic directories are world-searchable
precisely so path resolution can traverse onto them and cross a mount; the
execute bit there is load-bearing, not decorative.

**[[inv-i32]]** — the environment's 64-variable and 4096-byte-per-value ceilings
are this invariant in its smallest form, and the ramfs table cap is the same idea
applied to a boot-time input.

**[[inv-i33]]** — the boot root seeds its own name at birth, which is the one
place in the tree where a Spoor's recorded path is a root rather than an
accumulation.

## Error paths

Returning nothing and continuing: an absent or malformed archive (the filesystem
is simply empty); an archive with more entries than the table holds (the load
truncates — and **says so**); a random pull that finds no device, fails to
negotiate, cannot allocate, times out, or returns only zeroes.

Returning failure to the caller: a byte read of any of the three directories; a
write to the read-only boot filesystem; an environment operation naming an id
that no longer exists; a create anywhere but the environment root; a random read
before the source is ready; a first directory entry too large for the caller's
buffer, which must be an error rather than zero because zero already means
end-of-directory.

Ending the world: an environment freed twice.

## Performance

The archive is parsed once at boot. Environment lookups are linear over 64 slots
and enumeration is linear per step, so a full listing is quadratic in a bounded
64 — irrelevant at this size, and worth knowing before anyone raises the bound.
Random reads are a buffer copy, with a re-key every 984 bytes and a device
round-trip every megabyte.

## Prosecution

- **The random buffer's first fill is not secret, and only boot ordering hides
  that** (below). Any new consumer of randomness placed earlier in boot would
  serve a constant. This is the property to re-establish whenever boot order
  moves.
- The readiness gate must keep counting *only* unobserved sources. Admitting the
  bootloader seed would make a key derivable in part from a published address.
- The all-zero rejection must stay. It is the only thing standing between an
  incoherent transfer and a source that believes it is seeded.
- Both bounds on the device poll must stay. Removing the real-time one
  re-introduces a failure that depends on host speed; removing the iteration one
  lets a stopped clock hang the machine.
- The environment's ids must stay monotonic and never reused, and the minted
  device number must stay stamped at walk time — the identity it prevents
  colliding is consumed by a cache in another subsystem that has no way to
  detect the collision itself.
- The environment's flat-block render must keep **skipping** entries it cannot
  encode rather than emitting them (below).
- The ramfs table's pointers into the archive mean the archive can never be
  freed; anything that frees it must first copy the names and data out.
- Directory cursors must stay strictly increasing and never zero.

## Seams

The boot archive is still never freed. Environment sharing across a fork is
reserved but not built — a child always gets a copy. There are no directories
inside either filesystem: the ramfs is flat, and the environment is one level.

## Caveats

- **The random buffer's first fill is a compile-time constant.** A re-key
  generates its buffer *from the cipher's current state*, and on the first call
  that state is the zeroed one it was born with — a public value. The seed is
  mixed only into the first 40 bytes, which become the new key and are then
  erased; the remaining 984, which are the ones marked available to serve, are
  the unkeyed keystream and are identical on every boot of every machine. What
  prevents them reaching anyone is that a second re-key always runs before the
  first consumer — and that second re-key happens unconditionally, whether or not
  the strong pull succeeded. **The exposure is inverted from what one would
  expect**: on hardware *without* a CPU generator the readiness gate is still
  closed in that window, so reads fail safely, while on hardware *with* one the
  gate is open and a read in that window would return the constant. Verified: the
  first re-key runs from the zeroed state; the seed reaches only the first 40
  bytes; the servable window is the remaining 984; the second re-key is
  unconditional and precedes every consumer. Not verified as reachable — no
  consumer exists in that window today, which is why this is a structural note
  and not a live defect. No test can see it: every random test runs after boot,
  by which time the buffer is secret.
- **Two documents state that seeking to the end of an environment file works,
  and it does not.** Whether positioned reads and seeks are permitted is a
  separate, explicit flag on each Dev, introduced precisely because an earlier
  version inferred it from the presence of a metadata handler and got it wrong.
  The environment Dev added the metadata handler and did not set the flag, so the
  seek is refused before it ever reaches the size lookup. The guard did exactly
  its job — and the code comment and the reference document both record the
  belief the guard exists to falsify, which makes the error look corroborated.
  Verified: the flag is absent; the refusal precedes the size lookup; both
  documents claim otherwise. The underlying operations do honour offsets
  correctly, so the flag appears simply to have been forgotten.
- **A stale restatement of the same constant, twice, in two places.** The ramfs
  table cap was raised from 32 to 64 to 128 to 256 over the project's life; the
  comment block explaining the raises is meticulous, and a second comment thirty
  lines below still states the previous value. The reference document states the
  *original* value, and contradicts itself on the test count in two different
  sections. **The comment that gets corrected is the one the change was about;
  its restatement elsewhere is not** — the same failure in prose and in code, for
  the same reason.
- **A defensive fallback whose justification has expired.** The ramfs supplies a
  default file mode when the archive's is absent, justified by the claim that the
  archive generator always emits one fixed mode. It no longer does — it preserves
  each source file's real permissions, deliberately, so that executables carry
  their execute bit. The fallback is still unreachable, but the reason given for
  believing so is void.
- **An inherited environment handle reports its parent's device number.** The
  number is stamped onto the handle at walk time from the walking process; a
  child inheriting an open handle re-resolves its *contents* against its own
  environment but keeps the stamp, so the pair names the parent's file while the
  bytes are the child's — the one case the stamp cannot see. Not reachable from
  the cache the stamp protects, which always re-walks.
- **The flat environment block skips what it cannot encode, and this is the right
  trade.** Rendering the environment in the conventional flat form cannot express
  a name containing an equals sign or a value containing a zero byte, because
  those are the format's own separators — and emitting them raw would not
  truncate the answer but *corrupt* it, splitting one variable across two or
  folding one into another's value. Absence is a state every consumer already
  handles; a wrong value it cannot distrust is not. Notably, the same render goes
  to considerable trouble to be offset-aware precisely so it never drops a
  variable silently — so it accepts, for the unencodable case, exactly the
  failure mode it built machinery to avoid, because there the alternative is
  worse.
- **A comment promises a loud failure that would not occur.** The environment Dev
  documents its choice not to enforce permissions by asserting that enabling them
  would fail every open. It would not: the permission path consults the metadata
  handler, which reports the *calling* process as owner, so every check would
  pass and the change would be silently inert. The decision is right; the stated
  guard rail does not exist, and a comment promising noise where the reality is
  silence discourages the test that would reveal it.
- **The boot filesystem reports its truncation; the hardware enumeration next
  door does not** ([[sub-kernel-discovery]]). The difference is history: this
  one's comments record the cap silently truncating twice, once dropping files
  the boot expected. It learned because it was bitten.

## Provenance

Read from `kernel/devramfs.c` (662), `kernel/env.c` (452), `kernel/devenv.c`
(376), `kernel/random.c` (654), `kernel/chacha20.c` (102) and the three headers,
2026-08-02, at `631c8ade`. Cross-checked: the boot ordering of device
initialization against the strong re-seed, every caller of the random API, the
seek and positioned-read gates, the metadata stamp, the permission path's choice
of vtable slot, the device-registration consistency check, the archive
generator's mode handling, and the 40 registered tests across the five files.

[[chg-2026-08-16-seven-small-surfaces]] records this interval.
