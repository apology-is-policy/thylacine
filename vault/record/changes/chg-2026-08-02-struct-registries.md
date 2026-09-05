---
id: chg-2026-08-02-struct-registries
type: chg
title: "the struct registries -- each copy is pinned to itself, and nothing pins the copies to each other"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - abi-t-stat
  - abi-loom-ring
  - abi-ninep-wire
  - moc-boundary
established:
  - abi-t-stat
  - abi-loom-ring
  - abi-ninep-wire
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libt/include/thyla/syscall.h (t_stat: size + 8 of 14 offsets)"
  - "usr/lib/libthyla-rs/src/fs/metadata.rs (Metadata: size only, 0 offsets)"
  - "usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch (t_stat in fstat.c: size only)"
  - "usr/lib/pouch/patches/0019-pouch-stat.patch (t_stat in fstatat.c: size only)"
  - "usr/lib/pouch/patches/0021-pouch-pty.patch (pouch_tstat in ioctl.c: size only)"
  - "usr/lib/pouch/patches/0024-pouch-fs-process-wires.patch (raw t[88] + literal +40: none)"
  - "go-thylacine src/syscall/syscall_thylacine.go (Stat_t: none)"
  - "usr/lib/libthyla-rs/src/loom.rs (Sqe/Cqe/BufReg/Params: full offset sets; ring header: 5 bare literals)"
  - "stratum v2 include/stratum/9p.h (STM_9P_* extension range 124-141)"
depth: skeletal
created: 2026-08-02
---
Batch 26, taking up the second kind of registry pass (task #42) -- the
**struct layouts**, after batch 24 did the enumerated-value tables and batch
25 did the spec notes. Main had not moved since batch 25; the branch already
contained its tip. L-1 checked for the FOURTEENTH time and still absent.

Three ABIs, chosen because each is mirrored by hand in at least one other
tree: [[abi-t-stat]] (the file-metadata record, **seven** mirrors),
[[abi-loom-ring]] (five shared-memory structures, one mirror), and
[[abi-ninep-wire]] (a message-type number space shared with **another
project**).

**F1 -- EACH MIRROR IS PINNED TO ITSELF.** `struct t_stat` is declared eight
times: the kernel's, plus seven mirrors. Six carry
`_Static_assert(sizeof(...) == 88)`.

Trace a kernel growth to 96 bytes. Exactly **one** assertion fires: the
kernel's own, in the file already being edited. Every mirror's assertion
compares that mirror against a literal the same author typed into the same
file -- so all six still read 88, are still true of their own 88-byte
structs, and pass. The two unguarded mirrors pass by having nothing to check.
The build is green, and the kernel writes 96 bytes into seven 88-byte
buffers.

That is #100 exactly, and the guard set is **unchanged since #100**. The
kernel's assertion message states the obligation in full -- *"EVERY mirror
(libt, libthyla-rs, pouch patch 0010, the go-thylacine syscall.Stat_t) MUST
grow in lockstep"* -- and that sentence is the entire enforcement: a note to
the author, delivered while they are already editing the line, about six
other files. The historical detection was a boot segv plus a manual grep.
Task #43.

**F2 -- TWO MIRRORS HAVE NO GUARD AT ALL, AND ONE IS INVISIBLE TO THE GREP
THAT FOUND THE LAST STRAGGLERS.** The Go `Stat_t` has none (Go has no
`_Static_assert` and none was hand-rolled from `unsafe.Sizeof`). Pouch's
`faccessat` declares no struct -- `unsigned char t[88]` and a literal `+40`
to read the mode. It needs one field and open-coding it is defensible; the
cost is that the `88` and the `40` are invisible to any scan for `t_stat`.
So is pouch `0021`, which renames its copy `pouch_tstat`.

`CLAUDE.md`'s #100 addendum names six mirrors. There are seven -- `0024`
landed later -- and the sixth name it gives, gopls, is a *consumer* of the Go
mirror rather than an eighth layout. A change worked from that list is
complete by its own accounting and short by one file.

**F3 -- THE STRUCTURE BOTH SIDES WRITE IS THE LEAST PINNED OF THE FIVE.**
Loom's `loom_ring_hdr` carries the four head/tail words: `sq_tail` and
`cq_head` are user-advanced, `sq_head` and `cq_tail` kernel-advanced, and the
release/acquire pairing across them is the completion protocol. A field
mix-up there is not a crash but a silent corruption.

It has, on the kernel side, one offset assertion covering one of twelve
fields; and on the Rust side **no `#[repr(C)]` struct at all** -- five bare
constants (`HDR_SQ_HEAD = 0` through `HDR_FLAGS = 32`) read through an
`AtomicU32` view. All five are correct today. Nothing measures them, because
there is nothing for `offset_of!` to measure.

Within the same file, `Sqe` -- which only userspace writes -- carries eight
Rust offset assertions and two kernel ones. The pinning tracks how easy each
struct was to assert, not what a mistake would cost. Task #44.

**F4 -- THE DEFENSE WAS APPLIED TO THE AUDITED STRUCT, NOT TO THE CLASS.**
Those Rust `offset_of!` sets exist because a Loom-6d audit found them
missing. The finding's reasoning was general: *a same-size field reorder
leaves `sizeof` unchanged and silently shifts a byte-pinned ABI.* It is
exactly as true of `t_stat`, where it applies to **seven** mirrors instead of
one -- and `t_stat`'s Rust mirror still has only a size assertion, zero
offsets. The fix stopped at the scope of the audit that produced it. The
header, having no Rust type, was not measured either.

**F5 -- THREE PROSE CLAIMS THAT MAKE THE MIRROR SET LOOK SAFE.**
`metadata.rs` opens with *"Backed by struct t_stat (80 bytes, ABI-pinned)"*
above an 88-byte struct asserting 88. Pouch `0021` introduces its copy as
*"Mirror of the kernel struct t_stat (80 bytes, layout pinned by kernel
`_Static_assert`s)"* -- both halves wrong, immediately above an 88-byte
struct. Pouch `0019` repeats the second half: *"offsets pinned by the
kernel's `_Static_assert`s."*

That last belief is the load-bearing one. The kernel's assertions pin the
kernel's copy. A mirror that believes it inherits them is a mirror nobody
will think to check.

**F6 -- THE CLAIMED NATIVE MIRROR DOES NOT EXIST.** `p9_attr` (160 bytes),
`p9_setattr` (56) and `p9_statfs` (64) are declared in `9p_wire.h` as
internal decode targets. `LOOM_OP_GETATTR` / `STATFS` / `SETATTR` copy them
verbatim into a userspace buffer, which promoted them into ABI; twelve
assertions pin them, in `loom.c`, labelled as Loom's.

Both `loom.h` and `loom.c` say the layout is one *"the native
`libthyla_rs::loom` side mirrors at Loom-6d."* Loom-6d landed and the arc is
complete. There is no `P9Attr`, no `Statfs`, no `SetAttr` anywhere in
`libthyla-rs`. `op::GETATTR` and `op::STATFS` exist as opcode constants, so a
native program can submit one and then receives 160 bytes with no declared
type to decode. Twelve assertions pin an output ABI with zero declared
consumers on the side they were written for. Task #45.

Separately, the definition site carries no ABI marker at all. The guard works
-- `loom.c` includes the header, so a field add trips the build -- but a
developer extending `p9_attr` gets a failure citing Loom from a subsystem
they had no reason to open. The pin is in the consumer; the surprise is in
the definition.

**F7 -- THE COUNTERWEIGHT: THE ONE REGISTRY THAT COULD NOT USE AN ASSERTION
IS THE ONE THAT VERIFIES CLEAN.** The 9P message-type space is shared with
Stratum, and no compiler sees both trees -- so it is held by a document,
`docs/9P-EXTENSIONS.md`, which carries a single allocation authority, a burn
rule, a domain column, a next-free pointer, a recorded history, and backlinks
from every allocation site in **both** projects, including a pointer from
Stratum's own header across the project boundary.

Checked against all three enums this batch: Thylacine 124-133 + 140/141 +
142-145 with nothing at 146 or above; `ninep.rs` 142-145 only, correct per
the domain column; Stratum 124-139 + 140/141. **Accurate on every
allocation.**

Its one drift is in the harmless direction -- the "Defined in" column marks
128/130/132 as mirrored in `9p_wire.h` but not 124/126, which are also there.
The load-bearing columns are right; the descriptive one is stale. The rules
protect the half that matters.

And #371, the failure it was built from, is worth keeping for its shape: the
Weft quartet was allocated at 134-137 by an author who read `9p_wire.h`, saw
the mirrored Stratum block end at 133, and took the next free pair -- while
Stratum also assigns 134-139. The mistake was not carelessness. `9p_wire.h`
mirrors the Stratum ops Thylacine *issues* and not the others, so its highest
number reflects Thylacine's usage rather than the space's occupancy, and
looks equally authoritative either way. **A local enum in a shared number
space cannot show you the space.**

**THE THEME, AND THE SHAPE NOW THAT THERE ARE THREE.** Batch 24: the
assertions pin the values, nothing pins their description. Batch 25: the
models pin the mechanisms, nothing pins the model's own scope. Batch 26:
**each copy is pinned to itself, and nothing pins the copies to each other.**

All three are one shape -- **a guard whose subject is narrower than its
apparent claim.** `_Static_assert(sizeof(X) == 88)` reads like "X matches the
ABI" and means "X matches this literal." A TLA+ module reads like "this
mechanism is proven" and means "the modelled part is." A value assertion
reads like "this registry is correct" and means "these numbers are." In every
case the guard is sound and the inference drawn from its greenness is wider
than what it checked.

The 9P registry is the useful counter-case: it is *not* an assertion, so
nobody could over-read it, and the thing that replaced it -- rules, an
authority, backlinks, a burn policy -- states its own scope in prose. It is
the only one of the four registries swept across batches 24 and 26 that
verifies clean.

PROBE. Two, on the vault's own mirror guard (linter rule R6: a chg touching
an abi with N mirrors must carry at least N `mirrors-checked` entries).

**P1** -- name a `mirrors:` path that does not exist: **passes clean.** The
field is free text; nothing resolves it against the tree.

**P2** (control) -- **under-declare.** Cut `abi-t-stat`'s mirror list from
seven to three and the chg's obligation drops to three; the corpus lints
green. The rule scales to the count you *claimed*, not the count that
*exists*, so a note that under-reports its mirrors gets a weaker obligation
and a clean bill.

Which is the same defect as F1, in the vault instead of the tree, and the
same defect as `CLAUDE.md`'s six-of-seven list in F2. A count verified
against its own declaration is a local check that reads like a global one --
so the guard against under-counting mirrors is, itself, only as good as the
mirror count someone typed. Recorded rather than fixed: making R6 resolve
paths would make the vault's linter a build-tree checker, which is a
different tool.

LEDGER. Registries 4 -> **7** (four enumerated-value + three struct/wire).
Corpus 785 -> **789**. Boundary area 5 -> 8 abi notes. Absorption unchanged
at 46/101/147 -- deliberately; `107-loom.md` and the `t_stat`-bearing
reference documents now have somewhere to be absorbed *to*, which is the
point of a registry pass, but absorbing them is its own pass.
