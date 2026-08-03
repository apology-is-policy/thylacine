---
id: chg-2026-08-03-libthyla-rs-sweep
type: chg
title: "the native runtime — a plane where being wrong is free, and two claims that took the offer"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-libthyla-rs
  - moc-userspace-runtime
  - moc-userspace
  - sub-kernel-burrow
established:
  - sub-libthyla-rs
  - moc-userspace-runtime
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 37, and the first of the userspace sub-arc: `usr/lib/libthyla-rs`, the
runtime every authored Thylacine program links. Main had moved to `2f7cbc83`;
merged before starting. L-1 absent on the TWENTY-FIFTH check.

**#57 IS NOT A BATCH, SO IT WAS DECOMPOSED BEFORE THE FIRST NOTE WAS WRITTEN.**
Its description dates from batch 28 and undercounts: it names nine areas, but
`usr/lib` is not an area — it is EIGHT separate libraries (libthyla-rs 13.3k,
kaua 4.0k, parley 3.9k, libdriver 3.9k, netdev 1.6k, corvus-crypto 1.5k, tls,
libtapestry, cornucopia). Counted properly, userspace is ~19 units holding
~93700 of the 97364 lines then unswept — very nearly all of what remains. It is
now #92-#98, grouped by design document and by what a reader would ask about
together, with libthyla-rs first because every other native program links it.

**THE PLANE IS DIFFERENT, AND THE NOTES HAVE TO SAY SO.** This is the first
sweep of a surface that is **not a privilege boundary**: client code over a
frozen ABI, where the kernel validates every argument and a library bug
corrupts only its own caller. The Loom-6d audit had already said as much about
the same crate. So the dossier is `audit: light`, its Invariants section says
*composes with* rather than *enforces*, and a new area MOC
([[moc-userspace-runtime]]) carries the consequence: **on a plane where nothing
is load-bearing, a claim can be wrong indefinitely without anything failing.**
Both findings are that shape.

**MEASURED SOUND, and one of the checks was the interesting one.** 29 files;
`lib.rs` stays with [[sub-kernel-syscall-abi]] (it genuinely IS the mirror), so
the dossier claims the 28 above it and states the boundary. The prologue does
ONE auxv walk servicing two tags. The spawn builder's pointer-lifetime
reasoning is meticulous and correct, including the subtle part — an owned local
drops at scope end, not at NLL last-use, which is what keeps a raw pointer into
it valid across the syscall.

The check worth naming: `thread.rs` claims that the per-thread exit syscall
"from any Thread (incl. the `#[panic_handler]`)" terminates the whole Proc
cleanly rather than extincting the kernel — the hazard the ported side's abort
override carries. Batch 35's whole lesson was that a doc can describe a
stronger guarantee than the code, so this was verified rather than trusted:
`exits()` does count live peers and does call `proc_group_terminate`. **It is
true.** A comment that names its dependent consumer and is correct about it —
the opposite of batch 35's getrandom, where the comment cited a wrapper's
documented behaviour that was false for the call being made.

**F1 -- A FEATURE DECLARED IMPOSSIBLE FOR A REASON THAT EXPIRED, AND THE
CONSUMER WHO NEEDED IT WROTE IT TWICE.** `Stdio::Null` returns
`NotImplemented`. Three places say why:

> NOT IMPLEMENTED at v1 (no kernel /dev/null analog whose fd we can supply
> without an extra open)

That analog has existed since #57b: `devdev` has a `null` leaf — reads EOF,
consumes writes, world-rw — and joey boot-mounts `/dev`, extincting if the
mount fails. `devdev.c`'s own comment cites `clang++ < /dev/null` as a working
case, so the ported side already depends on it.

The live consumer is the driver supervisor, which needs exactly this and opens
the device itself, in a byte-identical three-line closure written out **twice**
in one file, each with its own failure arm:

```rust
let open_null = || OpenOptions::new().read(true).write(true).open("/dev/null");
```

That is the body `Stdio::Null` would have. **This is the arc's stale-reason
shape with the latency removed**: #89 and the L-6a refusal were both unreached,
waiting for a caller class that had not arrived. This one arrived, needed the
feature, was told it was impossible, and implemented it locally — and the
library still says it cannot be done. Task #99.

**F2 -- THE MORE PRECISELY THE ACCESSOR DOCUMENTED THE CONTRACT, THE MORE
PRECISELY IT BECAME WRONG.** Three statements about one field:

- `handle.rs`, generally: `rights()` returns "the rights bits the kernel
  granted at handle creation."
- `file.rs`, specifically and CORRECTLY about the kernel: since A-3b the
  rights are derived from the open mode — read-only yields read, create yields
  write, read-write yields both.
- both of `File`'s constructors: record `READ | WRITE | TRANSFER`
  unconditionally, and say so locally — "the userspace Handle rights are a
  hint."

So a read-only File reports rights it does not hold, and the most specific of
the three statements is the most precisely wrong: it names the exact table its
own file's constructors decline to implement, sixty lines above the disclaimer.
Latent — the handle accessor has one caller (the file accessor re-exporting
it), and the file accessor has none. Task #100.

**THE COUNTERWEIGHT IS A DESIGN RULE, AND IT IS THE SAME ONE BATCH 36 FOUND ON
THE OTHER SIDE OF THE BOUNDARY.** The hardware wrappers preserve
[[inv-i5]] — hardware handles never cross a Proc — by **not having a method
that could violate it**. Not one that checks and refuses: one that does not
exist. `!Send` and `!Sync` come free from the raw-pointer fields. The JIT
wrapper does the same, gating its capability once at construction and checking
nothing afterward.

Batch 36 found the kernel reaching this conclusion independently on the same
JIT surface ("the capability is checked once, at mint, and the object type
carries it after"). Two layers, written at different times, converging on
**an operation you cannot express beats one you remember to check** — because
forgetting a check is silent, and forgetting to add a method is impossible.
That is now the best-supported design claim in the vault: two independent
instances, on opposite sides of the syscall boundary.

Also sound and worth recording: the error decoder's catch-all carries the raw
errno and prints it, which is what makes the registry drift of #34 a
degradation (an unknown-with-its-number) rather than a mislabelling; an absurd
negative return saturates instead of wrap-casting onto a real errno; and the
allocator's init-race loser spins in userspace whose liveness rests on the
death check being on **both** EL0 return tails — the same asymmetry
[[seam-el0-irq-tail-no-notes]] records for note delivery, load-bearing here in
the direction that works.

**AND A PERFORMANCE FACT THE KERNEL SIDE HAS ALREADY PAID FOR.** The largest
read this library will issue is 8 KiB; read-to-end uses 1 KiB. The kernel
transfer maximum is 128 KiB — raised from 4 KiB as a *measured* throughput fix,
on the finding that a compiler build's reads were overwhelmingly one 4 KiB
chunk against a much larger negotiated message size. The native side of that
lift is unclaimed: a native program reading a megabyte through read-to-end
issues about a thousand syscalls where eight would do. Nothing in the crate
connects its buffer sizes to the kernel bound.

**REFLEXIVE, and small this time.** Main's #130 landed while this batch ran and
touched `kernel/syscall.c`, swept last batch — so the notes describing it were
re-checked rather than assumed. Nothing read false: the free-decision protocol
[[sub-kernel-burrow]] and [[lock-burrow]] describe is untouched. The one gap
was completeness — #130 adds `burrow_unmap_reporting`, and the Contract listed
only `burrow_unmap`. Added, with the reason it exists: a caller cannot compute
"did this drop free the pages" from anything visible beforehand, so the
operation reports its own effect. (Which is #130's own lesson, and it is
already the R2 addendum pin.)

**PATTERN, FOURTEEN BATCHES.** b32 the guard is right about the case it was
written for; b33 the reason was never written; b34 the reason was written but
not as a precondition; b35 the doc described a stronger guarantee than the code;
b36 the fix was written WITH its bug report and applied to one of two identical
sites; **b37 the claim is simply false, and nothing has failed because nothing
has read it.**

That is the plane's characteristic failure, not a coincidence of two findings.
On a privilege boundary a wrong claim is eventually contradicted by a fault. In
a library over a validating kernel there is no such feedback: the kernel goes
on being right underneath, so `Stdio::Null`'s stale reason and `File::rights()`'
unimplemented table can both persist indefinitely, and the only thing that
finds them is somebody reading the code against the world it describes.

LEDGER, read off the rendered view rather than predicted. Corpus 826 -> **829**
(three notes, not the usual two — this batch opens an area, so a MOC lands with
the dossier; the first draft of this line said 828, predicted from a render
taken before the chg note existed, which is the b34 lesson recurring inside the
sentence that cites it).
Coverage 170 -> **198 owned of 421**, 40% -> **47%** — the largest single jump
of the arc, and unswept lines fall 97364 -> **87022** (-10.6%). `usr/libthyla-rs`
goes 1/28 to **29/0**, joining the fully-swept group. `kernel` unchanged at
110/22; `arch` at 34/4.

**And after two batches of the ledger telling on itself, the two metrics agree.**
b35 moved them in OPPOSITE directions (a correction that made the ledger more
honest also made its headline look better); b36 moved lines 8% with the
percentage flat; b37 moves both together, +7 points and -10.6%. Three
consecutive batches, three different relationships between the same pair — which
is the argument for keeping both rendered side by side rather than choosing one.
`usr/lib` is unchanged at 3/38/16643, because the census counts libthyla-rs as
its own area; what remains under that name is kaua, libdriver, parley, netdev,
corvus-crypto, tls, libtapestry and cornucopia — the #94/#95/#96/#97 material.
