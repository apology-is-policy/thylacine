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
design: ["docs/ARCHITECTURE.md", "docs/VIVARIUM.md", "docs/DISTRO.md"]
created: 2026-08-03
updated: 2026-09-06
---
## Purpose

Turn untrusted bytes into a structured, validated description of what to map —
or refuse them. It parses; it does not map. That separation is the file's best
property: every hostile-input question is answered here, in a function that
allocates nothing, takes no lock, touches no address space, and returns a plain
struct.

## Contract

`elf_load(blob, size, out)` returns `ELF_LOAD_OK` and fills `out`, or one of
**twenty-four** distinct negative codes (measured; the prose long said
twenty-two, undercounting by one before D-2 even added its own). The granularity
is deliberate — one code per rejection class, so a test says which rule fired
rather than that something did.

Two preconditions the caller owns: `blob` must be 8-byte aligned (the struct
cast is undefined otherwise, and the sanitizer traps it), and `size` is the
extent the file's *contents* must fit within — which is not always the extent of
the buffer. See Mechanism.

Accepted: `ET_EXEC` (absolute `p_vaddr`) **or `ET_DYN`** — a PIE, placed at
`ELF_PIE_LOAD_BIAS` (DISTRO D-2) — `EM_AARCH64`, little-endian, 64-bit,
`ELFOSABI_NONE` or `ELFOSABI_GNU`. Refused: `PT_INTERP` (still, HERE — but see
below), `PT_DYNAMIC` **on an `ET_EXEC`** (a PIE carries one legitimately;
accepted and never processed), executable stacks, any segment both writable and
executable, and a biased PIE segment that leaves the window (`ELF_LOAD_PIE_OOB`).

**`PT_INTERP` is refused here, yet the system is no longer static-only.** The
loader loads exactly one image and runs no interpreter; DISTRO D-4's
rewrite-to-ldso route reads `PT_INTERP` at the *vivarium exec chokepoint* (via
`elf_read_interp`, below) and restarts resolution on the interpreter — so what
reaches `elf_load` is the interpreter itself, which carries no `PT_INTERP` of its
own, and this reject stays correct on both sides of D-4.

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

**The PIE bias is the one number D-2 added, and it enters in a single place.** An
`ET_DYN` image's `p_vaddr` are offsets from a base the loader chooses; `elf_load`
adds `ELF_PIE_LOAD_BIAS` (512 MiB) to `e_entry` and to every `PT_LOAD`'s `vaddr`,
so everything downstream — the segment mapper, the file-backed/eager split, the
`AT_PHDR` translation — reads FINAL addresses and needed no change. `bias` is 0
for `ET_EXEC`, so that path stays byte-identical; the W^X gate, the page-sharing
refusal, and the alignment terms are unchanged, only the numbers moved. The bias
is 64 KiB-aligned on purpose: stock aarch64 ELFs carry `p_align` 0x10000, so a
multiple of it preserves the gABI `p_vaddr == p_offset (mod p_align)` congruence —
which keeps a segment's file-offset alignment, and therefore its eligibility for
the shared file-backed arm, the same biased or unbiased. The biased span is
bounded inside `[BIAS, ELF_PIE_LOAD_LIMIT)` (`ELF_LOAD_PIE_OOB`), so the loader
states what it will and will not place rather than leaning on a general allocator
rule to catch a hostile `p_vaddr`. It is ONE constant deliberately; per-exec
randomization is a recorded I-16-adjacent seam.

**The `PT_INTERP` walk is now shared, not duplicated (D-4 / #215).**
`elf_read_interp` is the bounded walk `elf_brand_hint` used to carry privately; D-4
promoted it because the rewrite route ACTS on the extracted path — it resolves and
execs it — and a second copy of "is this offset inside the prefix, is this string
terminated" would be a second place for that judgement to drift (#140 is the
standing demonstration). Promoting it to a second PUBLIC parser of hostile bytes
made it inherit `elf_load`'s alignment preconditions: both the buffer alignment
(R5-G F61 — the sibling's guard #215 gave the new walk) and the
attacker-controlled `e_phoff` alignment (R5-G F62). It is pure, answers 0 (never a
partial answer) for every unreadable / unterminated / empty / oversized case, and
caps the path at `ELF_INTERP_MAX` (255; a real interpreter path is under 32 bytes).

## Data structures

`struct elf_image` — the entry point, the phdr table's location for `AT_PHDR`,
and up to sixteen `elf_load_segment` records. Flat, fixed, no pointers into the
blob, which is what lets `exec` keep using it after the header buffer is freed.
D-2 added two fields: `load_bias` (0 for `ET_EXEC`, `ELF_PIE_LOAD_BIAS` for a
PIE) and `type` (the `e_type`, diagnostics). `entry` and every `segments[].vaddr`
are FINAL — they already carry the bias, and no consumer adds it. `AT_ENTRY`
(auxv tag 9) reports `entry`; it is NOT what makes stock ldso work (musl writes
it itself on the direct-invocation branch, #186), but the v1.x in-kernel
dual-image lift would need it.

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
just not the load-bearing layer, and its own file header says otherwise. The D-2
PIE bias does not touch this leg: the W^X check reads `p_flags` above the bias
arithmetic, so it is byte-identical whether a segment is fixed or relocated.

## Error paths

Twenty-four codes, all negative, all reached before any state is mutated. `out`
is zeroed regardless. `ELF_LOAD_HAS_DYNAMIC` narrowed with D-2 — it now fires
only on a `PT_DYNAMIC` in an `ET_EXEC`, since a PIE carries one legitimately — and
`ELF_LOAD_PIE_OOB` is the code D-2 added. Nothing here can fail an allocation or
block, so every error is a pure classification.

`ELF_LOAD_HAS_INTERP` used to travel badly: `exec` collapses every non-OK return
to `-1`, so a dynamic binary and a corrupt one failed identically. D-4 changed the
destination, not this return — the vivarium exec chokepoint now reads `PT_INTERP`
with `elf_read_interp` BEFORE resolution would reach this reject and rewrites the
exec onto the interpreter, so a Linux dynamic binary RUNS. Outside a vivarium the
collapse still stands, which is what the brand hint below was built to soften.

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

- **Dynamic linking is served by rewrite, not by a relocator here.** D-2/D-4
  made stock dynamic binaries run — a PIE loads at the bias, and a `PT_INTERP`
  binary is rewritten onto its interpreter at the vivarium exec chokepoint — but
  the loader itself still processes no `PT_DYNAMIC` and runs no interpreter. A
  true in-kernel relocator, or the dual-image `PT_INTERP` lift, is the recorded
  v1.x seam (`AT_ENTRY` is already emitted for it).
- Per-exec PIE base randomization is an I-16-adjacent seam: today the bias is one
  constant, and the natural shape when it lands is a parameter on `elf_load`.
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

DISTRO extended the accepted set: D-2 added `ET_DYN`/PIE placement + `AT_ENTRY`
(`ELF_PIE_LOAD_BIAS`, `ELF_LOAD_PIE_OOB`, the `load_bias`/`type` fields), D-4 the
shared `elf_read_interp` for the rewrite-to-ldso route, and #215 gave that walk
its sibling's alignment guards. [[chg-2026-09-06-elf-distro-dynamic]].

## Tests

`elf.*` is the densest negative-test suite in the tree — roughly forty
assertions, most of them a hostile or malformed header proving a specific code
fires. `elf.brand_hint` adds eleven more, including the one that guards the
`EI_OSABI` omission.

## Referenced by

[[moc-kernel-execution]] · [[inv-i12]] · [[sub-kernel-exec]] ·
[[sub-kernel-image]]
