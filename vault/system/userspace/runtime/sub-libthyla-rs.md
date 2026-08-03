---
id: sub-libthyla-rs
type: sub
title: "libthyla-rs — the native runtime: RAII over the handle table, one error type, and two invariants it makes unexpressible"
parent: moc-userspace-runtime
code:
  - usr/lib/libthyla-rs/src/alloc.rs
  - usr/lib/libthyla-rs/src/cap.rs
  - usr/lib/libthyla-rs/src/env.rs
  - usr/lib/libthyla-rs/src/err.rs
  - usr/lib/libthyla-rs/src/fs/dir.rs
  - usr/lib/libthyla-rs/src/fs/file.rs
  - usr/lib/libthyla-rs/src/fs/metadata.rs
  - usr/lib/libthyla-rs/src/fs/mod.rs
  - usr/lib/libthyla-rs/src/fs/options.rs
  - usr/lib/libthyla-rs/src/fs/path.rs
  - usr/lib/libthyla-rs/src/handle.rs
  - usr/lib/libthyla-rs/src/hardware.rs
  - usr/lib/libthyla-rs/src/identity.rs
  - usr/lib/libthyla-rs/src/io.rs
  - usr/lib/libthyla-rs/src/jit.rs
  - usr/lib/libthyla-rs/src/loom.rs
  - usr/lib/libthyla-rs/src/net.rs
  - usr/lib/libthyla-rs/src/ninep.rs
  - usr/lib/libthyla-rs/src/notes.rs
  - usr/lib/libthyla-rs/src/poll.rs
  - usr/lib/libthyla-rs/src/process.rs
  - usr/lib/libthyla-rs/src/rand.rs
  - usr/lib/libthyla-rs/src/sched.rs
  - usr/lib/libthyla-rs/src/territory.rs
  - usr/lib/libthyla-rs/src/thread.rs
  - usr/lib/libthyla-rs/src/time.rs
  - usr/lib/libthyla-rs/src/torpor.rs
  - usr/lib/libthyla-rs/src/weft.rs
audit: light
guarded-by: [inv-i5, inv-i12, inv-i32]
validated-by: [prose, gate-interactive, gate-smp]
locks: []
hazards: []
abis: [abi-errno, abi-handle-rights]
design:
  - "docs/UTOPIA-SHELL-DESIGN.md section 15"
  - "docs/ARCHITECTURE.md section 3.5"
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

The runtime every *authored* Thylacine program stands on. The native half of
the split in ARCHITECTURE.md section 3.5 — code written within Thylacine links
this and calls the kernel directly; code ported from elsewhere goes through
[[moc-pouch-seam]] instead. The shell, the editor, the compositor, the
identity daemon, the driver supervisor and all sixty coreutils are consumers.

**What this dossier covers, precisely.** The crate is 29 files. One of them —
`lib.rs`, the raw `t_*` SVC wrappers and the mirrored constants — is the Rust
copy of the syscall ABI and is described by [[sub-kernel-syscall-abi]], which
owns it. The subject here is the other 28: the typed layer *above* the
wrappers. So the question this dossier answers is not "what can a Thylacine
program ask the kernel to do" but **"what does the library add, and where does
what it adds disagree with what it says it adds"**.

## Contract

A native binary does three things and gets a runtime:

1. depends on the crate, which supplies `_start` (kept alive across the rlib
   boundary by the linker script's `ENTRY`);
2. defines `#[no_mangle] pub extern "C" fn rs_main() -> i64`;
3. declares a `#[global_allocator]` — conventionally `alloc::ThylaAlloc`.

The third is not optional even for a program that never allocates: the symbol
must resolve at link time. It is also deliberately *not* declared by the crate,
because Rust permits exactly one per binary and the identity daemon already
brings its own static-BSS allocator; declaring one here would break that link.

`_start` calls a small Rust shim that stashes `(argc, argv)` for `env::args()`,
walks the auxv once, then calls `rs_main` and tail-calls the exit syscall with
its return value. A panic exits with status 1.

## Mechanism

### The prologue does one auxv walk and services two tags

Before `rs_main` runs, the shim walks past argv and envp to the auxv and reads
it once, servicing both tags the runtime cares about:

- the vDSO clock page (a Thylacine-private tag, deliberately outside the
  System V range so a ported libc ignores it) — validated for magic, version
  and a non-zero frequency, then cached in a static;
- the standard hardware-capability word, which is republished into
  `compiler_builtins`' `__aarch64_have_lse_atomics` byte when the CPU reports
  FEAT_LSE.

That second one is the userspace twin of the kernel's boot-time atomics
patcher: same question (does this core have LSE?), same answer source (the ID
registers, via the kernel), different mechanism (a byte the outline helpers
branch on, rather than rewritten instructions). It is per-binary rather than
shared because the symbol has hidden visibility, so there is no cross-binary
copy to race. The runtime exposes both `hwcap()` and `have_lse_atomics()`
specifically so a prover can assert they agree — a disagreement means either a
broken auxv walk or a false positive that would fault on an ARMv8.0 core.

Everything here lands in statics, because it runs before the allocator exists.

### Ownership is RAII, and the constructor is the gate

`Handle` owns one slot in the calling process's handle table and closes it on
`Drop`. Every resource-bearing type in the crate composes one: `File`,
`Notes`, `Mmio`, `Irq`, `Dma`, `Child`'s pipe ends.

The discipline that makes this more than convenience is that `Handle`'s
constructor is crate-private. External code cannot mint one from an arbitrary
integer, so the only way to hold a handle is to call a typed constructor that
went through a kernel call — which is where rights are set. `Handle` is
deliberately not `Clone`, because the kernel has no duplicate-with-same-rights
operation and a `Clone` impl would imply one.

There is exactly one public escape hatch, `File::from_raw_fd`, and it is
`unsafe` — the seal is the keyword rather than the privacy.

### One error type, decoded in one place

Every fallible operation returns the crate's `Result`, and every syscall return
is decoded by a single function rather than at each call site. Three behaviours
in it are worth naming, because each is a decision rather than a default:

- **A bare `-1` decodes to an I/O error, not a permission error.** The errno
  registry keeps its permission code at 1, so `-T_E_PERM` and the generic
  failure sentinel are the same bit pattern. Reading `-1` as "not permitted"
  mislabelled every flat-error failure — a missing file, a bad descriptor — as
  a permission problem, so it maps to the generic I/O error instead, matching
  the ported side's choice.
- **An unrecognised errno survives as itself.** The catch-all variant carries
  the integer, and its `Display` prints it. This is what makes the registry
  drift in this crate a degradation rather than a silent mislabelling: an
  errno the crate has not enumerated arrives as an unknown-with-its-number,
  never as the wrong known variant.
- **An absurd negative saturates instead of wrapping.** A return below the
  32-bit range would wrap-cast onto a real errno value; it saturates to the
  maximum so the kernel bug surfaces as a visibly impossible error.

### The heap is one lazy reservation, initialized once

The global allocator takes a single 4 MiB *lazy* anonymous region on the first
allocation and subdivides it locally. Lazy is the operative word: the region is
reserved, not committed, so a program that allocates a few kilobytes has a few
kilobytes resident, and a program that never allocates never makes the call.

Initialization is a three-state atomic. The first caller wins a
compare-exchange, makes the syscall, initializes the heap, and publishes with a
release store; a loser spins on an acquire load until it sees the published
state. If the reservation fails there is no recovery, so the winner exits the
process — which is also what unblocks the losers, by killing them.

### Two invariants are enforced by absence rather than by checking

The hardware wrappers preserve hardware-handle non-transferability
(**[[inv-i5]]**) *by not having a method that could violate it*. There is no
transfer operation on `Mmio`, `Irq` or `Dma` — not one that checks and refuses,
one that does not exist. The types are additionally `!Send` and `!Sync` for
free, because they hold raw pointers.

The JIT surface does the same thing one level up: the capability to emit
executable code is checked once, when a code region is created, and the publish
and destroy calls check nothing — they are reachable only through a region that
the gated constructor produced.

Both are the same shape, and it is the shape worth copying: **an operation you
cannot express is stronger than one you remember to check**, because forgetting
a check is silent and forgetting to add a method is impossible. The kernel
reaches the same conclusion independently on the same JIT surface
([[sub-kernel-syscall-dispatch]]), which is some evidence it is the right one.

## Data structures

None crossing a boundary; every ABI record belongs to
[[sub-kernel-syscall-abi]]. The crate's own types:

- **`Handle`** — a slot index plus a rights word. The rights word is the
  problem described in Caveats.
- **`Rights`** — a bitflag newtype over the kernel's right bits, hand-rolled
  to stay dependency-free. It carries its own copy of the all-rights literal,
  which is one more unpinned copy of a bound the kernel also states as a
  literal.
- **`Error`** — a flat enum of fifteen errno-mapped variants, a catch-all
  carrying an integer, and two library-only variants for a read that ended
  early and a write that stopped making progress. No allocation: variants are
  unit or carry one integer, and `Display` uses static strings.
- **`Stdio` / `PreparedStdio`** — the spawn plumbing. The prepared form splits
  what the parent must hold *through* the syscall from what it keeps *after*,
  which is the distinction that gets end-of-file semantics right.
- **`CodeRegion`** — the two aliases of one dual-mapped region. Its mirrored
  record is the only one in the crate pinned with per-field offset assertions
  rather than a size assertion alone.

## Concurrency

The crate has no locks of its own. Three concurrency facts matter:

- **The allocator's init spin terminates for a reason worth stating.** A loser
  of the init race spins in userspace with no syscall. If the winner fails and
  exits, the spinner is not sleeping and would never notice — except that the
  exit cascades a group termination, and the resulting inter-processor
  interrupt lands the spinner in the kernel, where the die-check on the
  interrupt return tail terminates it. So the spin's liveness rests on the
  death check being present on *both* return tails, which it is; note delivery
  is the one that is present on only one ([[seam-el0-irq-tail-no-notes]]).
- **A panic in any thread kills the whole process, and this is checked.** The
  panic handler calls the per-thread exit syscall, not the group exit. That
  used to be a hazard — the same shape as the ported side's abort override —
  and is not one now: the kernel's per-thread exit counts live peers and
  cascades a group termination when it finds any. The thread module states this
  and names the panic handler as the case that depends on it. It is true; the
  kernel does count and cascade.
- **The device wrappers are single-thread by construction.** Raw pointer
  fields make them neither `Send` nor `Sync` without anyone writing a bound.

## Invariants enforced

**None.** The crate is client code over a frozen ABI; the kernel validates
every argument, and a buggy program corrupts only its own state. What the crate
does is *compose* with several invariants, in one of two ways:

**[[inv-i5]]** and **I-42** — composed by absence, as described in Mechanism.
The library cannot express the violating operation.

**[[inv-i32]]** — the allocator's lazy reservation is charged per page as the
heap is touched, so a native program's heap counts against its own page
budget as it grows rather than at reservation.

**I-2** and **I-6** (capability and rights monotonic reduction — neither has a
note yet) — composed by the crate-private handle constructor: rights are fixed
where a kernel call sets them and there is no public path to a handle that
skipped one. The spawn builder likewise defaults its capability mask to the
caller's own set and lets the kernel intersect.

**[[inv-i12]]** — the JIT wrapper's whole reason for existing is that no page
is ever both writable and executable; it hands out two pointers to one region
instead. The publish call between writing and calling is not an optimization:
without the cache maintenance the processor may fetch what the instruction
cache held before, which for a fresh region is a trap instruction and for a
reused one is the previous function.

## Error paths

Uniform: every module returns the crate `Result`, and every syscall return goes
through the one decoder. Two exceptions, one deliberate and one worth watching.

The deliberate one: the JIT module defines its own small error enum with
domain-specific names, and its own copies of three errno constants, rather than
reporting through the crate error type. Its catch-all preserves the raw value
the same way, so nothing is lost — but the error module's claim to be the type
"every libthyla-rs module reports through" has an exception that does not know
it is one.

The one to watch: the formatting macros — the crate's `print!` family — swallow
write errors by design, so a program whose output is optional does not fail
spuriously. A program that must know whether its output landed has to use the
write methods directly. This is documented at the macros and is easy to miss at
a call site, since the macro looks exactly like the one it is modelled on.

## Performance

**The single largest read the library will issue is 8 KiB, against a kernel
transfer maximum of 128 KiB.** The default trait implementation of read-to-end
uses a 1 KiB stack buffer; the buffering reader defaults to 8 KiB; the copy
helpers use 8 KiB. No `File` override raises any of them.

That is worth stating plainly because the kernel-side bound was raised *from*
4 KiB *to* 128 KiB as a measured throughput fix, on the finding that a
compiler build's reads were overwhelmingly one 4 KiB chunk against a much
larger negotiated message size. The native side of that lift is unclaimed: a
native program reading a megabyte through read-to-end issues about a thousand
syscalls where eight would do. The 1 KiB figure is a stack-frame choice rather
than a considered transfer size, and nothing in the crate connects it to the
kernel bound.

The one place the crate does take the fast path is time: the clock readers go
through the vDSO page when the auxv delivered one, computing the same split
form the kernel uses so the value is identical to the syscall's at the same
instant, and falling back to the syscall when the page is absent.

## Prosecution

- **A new resource-bearing type composes `Handle`.** It does not hold a bare
  descriptor integer, because then closing it is a thing someone has to
  remember.
- **A new handle-minting path is crate-private and sets rights where the kernel
  call sets them.** A public constructor taking a raw descriptor must be
  `unsafe`, which is the existing escape hatch's whole justification.
- **A new hardware or code-region type adds no transfer, no `Clone`, and no
  `Send`/`Sync` impl.** The invariants above are held by those absences; adding
  any of them silently converts a structural guarantee into a missing check.
- **A new module reports through the crate error type**, or states why it does
  not — there is one exception already and it is undeclared.
- **A new syscall wrapper decodes through the single decoder**, so the bare
  sentinel and the saturation behaviour stay in one place.
- **A new bulk-I/O buffer size is chosen against the kernel transfer maximum**,
  not against the neighbouring buffer.
- **A capability is checked at mint.** Prefer gating later operations on the
  object the gated constructor produced.

## Seams

- **The spawn builder cannot set a page budget.** The ABI record carries the
  field and the builder hardcodes it to inherit. Nothing native can raise or
  lower a child's budget without hand-building the record, which is the one
  structure the typed layer exists to avoid.
- **The heap does not grow.** It is one fixed reservation; a program needing
  more must attach its own regions. See Caveats for what exceeding it looks
  like.
- **Per-command environment and working directory are absent.** Environment is
  inherited wholesale — the ABI's reserved field is still reserved, and the
  environment arrived instead as a per-process filesystem. Working directory is
  a whole-namespace operation, so a per-spawn one needs a different surface.
- **The thread module is the raw surface only.** There is no closure-taking
  spawn: doing it needs stack allocation, closure marshalling and a join
  protocol, and the module argues it should wait for a consumer. None of the
  native programs is multi-threaded today, so the argument still holds.
- **Vectored and asynchronous I/O are absent** from the trait surface. The
  asynchronous path exists, but through the ring rather than through these
  traits.

## Caveats

- **`Stdio::Null` is unimplemented for a reason that expired.** Three places
  say the discard mode cannot be built for want of a kernel bit-bucket device.
  That device has existed since `/dev` became a mounted namespace directory —
  it reads end-of-file, consumes writes, is world-readable-and-writable, and
  the boot fails if the mount does. The ported toolchain already redirects from
  it. Meanwhile the one native consumer that needs discard — the driver
  supervisor — opens the device itself and passes it as a file, in a
  byte-identical three-line closure written out **twice** in one file, each
  with its own failure arm. This is the arc's stale-reason shape with a
  difference: it is not latent. The need was demonstrated, the workaround
  shipped, and the library still declares the feature impossible. Task #99.

- **A `File`'s rights word is a constant, and its accessor documents the table
  it does not implement.** The handle type's accessor is documented as "the
  rights bits the kernel granted". The file type's accessor is documented far
  more specifically and describes the kernel's actual derivation correctly —
  read-only opens yield read, create yields write, read-write yields both. Both
  of `File`'s constructors then record read-plus-write-plus-transfer
  unconditionally, and say so locally, calling the value "a hint". So a
  read-only file reports rights it does not hold, and the most precise of the
  three statements is the most precisely wrong. Nothing reads it today — the
  handle accessor has exactly one caller, the file accessor has none — so the
  wrong value is currently inert. Task #100.

- **Exhausting the heap is indistinguishable from a panic, and neither says
  anything.** There is no custom allocation-error handler, so an allocation
  past 4 MiB panics, and the panic handler exits with status 1 discarding the
  message. A failed initial reservation also exits 1. Three quite different
  failures — out of memory, a logic panic, and a runtime that could not start —
  are one exit status and no output. The panic handler's own comment scopes a
  richer path as future work; until then, a native program that dies has told
  its parent almost nothing.

- **One heap comment names the wrong bound of a near-miss pair.** The module
  header correctly names the reservation ceiling (one gigabyte) for the lazy
  call it makes; the constant's own documentation names the *eager* attach
  ceiling (256 MiB) instead. Both constants are real and distinct. The
  arithmetic quoted is right for the constant named, and the error is in the
  safe direction — it understates the available headroom fourfold — but it is
  the second consecutive sweep to find a documented constant that is the wrong
  one of a similar-looking pair, and the previous one was not safe.

- **The rights bitflag carries its own copy of the all-rights literal.** The
  kernel states that bound as an unpinned literal too, at six validation sites
  (#36); this is a seventh copy, in another language, pinned to nothing. The
  type deliberately permits bits outside it, on the argument that the kernel
  rejects them at creation — which is true, and means the type is a vocabulary
  rather than a validator.

## Provenance

[[chg-2026-08-03-libthyla-rs-sweep]].
