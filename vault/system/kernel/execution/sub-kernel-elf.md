---
id: sub-kernel-elf
type: sub
parent: moc-kernel-execution
title: "The ELF loader — a validator that refuses more than it accepts, and an advisory nothing asks"
code: [kernel/elf.c, kernel/include/thylacine/elf.h]
audit: hard
guarded-by: [inv-i12]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md", "docs/VIVARIUM.md"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

Turn untrusted bytes into a structured, validated description of what to map —
or refuse them. It parses; it does not map. That separation is the file's best
property: every hostile-input question is answered here, in a function that
allocates nothing, takes no lock, touches no address space, and returns a plain
struct.

## Contract

`elf_load(blob, size, out)` returns `ELF_LOAD_OK` and fills `out`, or one of
**twenty-two** distinct negative codes. The granularity is deliberate — one code
per rejection class, so a test says which rule fired rather than that something
did.

Two preconditions the caller owns: `blob` must be 8-byte aligned (the struct
cast is undefined otherwise, and the sanitizer traps it), and `size` is the
extent the file's *contents* must fit within — which is not always the extent of
the buffer. See Mechanism.

Accepted at v1.0: statically-linked `ET_EXEC`, `EM_AARCH64`, little-endian,
64-bit, `ELFOSABI_NONE` or `ELFOSABI_GNU`. Refused: dynamic (both `PT_INTERP`
*and* `PT_DYNAMIC`, separately), executable stacks, any segment that is both
writable and executable.

## Mechanism

Five stages, each a gate: identity bytes, machine and type, the program-header
table's own bounds, per-segment validation, and finally that the entry point
lands inside some loaded segment.

**The W^X check sits above the type switch, not inside the `PT_LOAD` case.** That
placement was an audit fix and it is the file's sharpest idea: the invariant is
made *type-blind*, so a future `PT_*` the loader has never heard of gets the
check for free and no one has to remember to add it. It is the exact inverse of
this arc's recurring finding — a guard deliberately made **wider** than the case
that motivated it.

The overflow discipline is uniform: every `a + b` against a bound goes through a
checked add, and the phdr-table span is a widening multiply. There is no place
where a 64-bit sum is compared without first asking whether it wrapped.

**The two meanings of `size`.** `elf_load` is called twice with materially
different intent. The blob path passes a buffer and its length. The file-backed
path passes a **16 KiB prefix** of the file and the **whole file's** length —
deliberately, because segment extents must be validated against the real file,
not against the header window. That works only because the caller separately
bounds the phdr table within what it actually read. The split is correct and
explained at both ends, but it means `size` is not "how many bytes you may
dereference" — the only reason the phdr walk stays in bounds is a check that
lives in another file.

## Data structures

`struct elf_image` — entry point, the phdr table's location for `AT_PHDR`, and
up to sixteen `elf_load_segment` records. Flat, fixed, no pointers into the
blob, which is what lets `exec` keep using it after the header buffer is freed.

`out` is **zeroed on entry**, another audit fix: a caller who ignores the error
code and reads the struct anyway gets defined zeros rather than partially-parsed
attacker bytes. The contract still says ignore it; the code no longer depends on
that.

## Concurrency

None. Pure function over a caller-owned buffer, no global state in the load
path. It is the one file in this area with nothing to say here, and that is a
design property rather than an omission.

## Invariants enforced

[[inv-i12]] — the ELF layer. A `PT_LOAD` with both `PF_W` and `PF_X` is
rejected outright, for every segment type, before anything else looks at the
header.

Worth being exact about what this leg is worth. It is *not* the gate that makes
W^X hold — that is `vma_alloc`, which refuses the same combination for every
mapping in the system regardless of where it came from. This one catches the
violation earlier and reports it precisely. Defence in depth, correctly built;
just not the load-bearing layer, and its own file header says otherwise.

## Error paths

Twenty-two codes, all negative, all reached before any state is mutated. `out`
is zeroed regardless. Nothing here can fail an allocation or block, so every
error is a pure classification.

The one code that travels badly is `ELF_LOAD_HAS_INTERP`: `exec` collapses every
non-OK return to `-1`, so a dynamically-linked binary and a corrupt one fail
identically from userspace. That is what the brand hint below was built to
soften.

## Performance

Bounded by `e_phnum`, capped at 256. No allocation, no I/O. Irrelevant next to
the page-ins that follow.

## Prosecution

On any change: that the W^X check stays **above** the switch, so a new segment
type inherits it; that every bound comparison keeps its overflow guard; that the
alignment precondition stays enforced rather than assumed; that `out` stays
zeroed on entry; and that any new caller of `elf_load` is explicit about which
of the two meanings of `size` it is passing — the file-backed path's
prefix-buffer-with-file-length pairing is safe only in combination with a bound
that lives in [[sub-kernel-exec]].

## Seams

- **Dynamic linking is refused permanently**, not deferred — the two rejections
  are policy, not gaps.
- **Non-page-aligned segments** are rejected by exec, not here; the ELF spec
  permits them.
- **A positive native brand** (a `.note.thylacine`-shaped marker) is the
  recorded v1.x seam that would let the brand hint answer in both directions.

## Caveats

**`elf_brand_hint` has no production caller.** It is a careful, pure, well-tested
advisory that reports whether a binary looks Linux-shaped, and its own header
states why it exists: *"an obvious mismatch ... earns a diagnostic and a clean
failure instead of a silent mis-decode."* Nothing outside the test suite calls
it, so no such diagnostic is emitted — a Linux dynamic binary exec'd outside a
vivarium gets `ELF_LOAD_HAS_INTERP` collapsed to a bare `-1`, which is precisely
the silent failure the function exists to prevent. The [[arc-vivarium]] arc is
complete.

It is the third dormant declaration found in three consecutive sweeps, after
the W^X checker and the note mask — but it differs from both in a way worth
recording: **no document claims it is wired.** A search of the design docs
returns nothing. So unlike its two predecessors it misleads no one; it is simply
finished work with no consumer. Its reasoning about why `EI_OSABI` cannot decide
a phenotype is correct and valuable, and there is a regression test that fails if
someone "improves" it by consulting that byte — a guard protecting a deliberate
non-decision inside a function that never runs. Task #62.

**The file header names `mprotect` as one of three W^X layers.** There is no
`mprotect` in this kernel — a search of the whole of `kernel/`, `arch/` and `mm/`
returns exactly one hit, this comment. The *absence* of any protection-changing
syscall is genuinely one of the mechanisms that makes [[inv-i12]] hold, but an
absence is not a layer, and naming it as one alongside two real checks reads as
an inventory. The same sentence omits `vma_alloc`, making this the sixth
document to do so. Folded into task #59.

**`ELF_LOAD_BAD_OSABI`'s comment says only `ELFOSABI_NONE` is accepted**; the
code accepts `ELFOSABI_GNU` too, and has to — the native toolchain emits it,
which is the same fact the brand hint is built around. A one-word fix, worth
taking with whichever pass touches the others.

## Provenance

P2-Ga, parse-only from the start. Hardened through the R5-G audit round, whose
fixes are still visible individually: the alignment precondition, the phoff
alignment check, the zeroed-`out` entry, the `PT_DYNAMIC` rejection, and the
W^X hoist above the switch. The brand hint arrived with [[arc-vivarium]] V-1.

The file's own header still describes the loader as parse-only pending a Phase 3
that would wire mapping and a Phase 5 that would add an exec surface. Both
arrived. The description is accurate about the function and stale about the
world around it.

## Tests

`elf.*` is the densest negative-test suite in the tree — roughly forty
assertions, most of them a hostile or malformed header proving a specific code
fires. `elf.brand_hint` adds eleven more, including the one that guards the
`EI_OSABI` omission.

## Referenced by

[[moc-kernel-execution]] · [[inv-i12]] · [[sub-kernel-exec]] ·
[[sub-kernel-image]]
