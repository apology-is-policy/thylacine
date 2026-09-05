---
id: abi-ninep-wire
type: abi
kind: wire
stability: append-only
title: "The 9P message-type space — a number registry shared with another project"
pinned-by:
  - "docs/9P-EXTENSIONS.md: the cross-project allocation authority (rules + burn policy + history + next-free pointer)"
mirrors:
  - "kernel/include/thylacine/9p_wire.h: P9_T*/P9_R* (the kernel wire enum)"
  - "usr/lib/libthyla-rs/src/ninep.rs: P9_T*/P9_R* (the codec netd serves with)"
  - "stratum v2 include/stratum/9p.h: STM_9P_T*/STM_9P_R* (the other project's enum)"
created: 2026-08-02
updated: 2026-08-02
---
## The surface

Every 9P message opens with the same 7-byte header — `u32 size`, `u8 type`,
`u16 tag` — and the `type` byte is a number space **shared between Thylacine
and Stratum**, two separately-compiled projects with independent enums.
`P9_NOTAG` (0xFFFF) and `P9_NOFID` (0xFFFFFFFF) are the reserved sentinels;
T/R pairs are adjacent even/odd, `R = T + 1`, by 9P convention.

Below 128 the space is 9P2000.L's, used as specified. Above it, three
allocators have written into one range:

| range | owner | in Thylacine's enum? |
|---|---|---|
| 124–127 | Stratum — `Tbind` / `Tunbind` | yes, mirrored |
| 128–133 | Stratum — `Tsync` / `Treflink` / `Tfallocate` | yes, mirrored |
| 134–139 | Stratum — `Tfadvise` / `Tpin` / `Tunpin` | **no** |
| 140–141 | shared — `Twalkgetattr` (POUNCE: Thylacine-designed, Stratum-implemented) | yes |
| 142–145 | Thylacine — `Tweft` / `Tweftio` (kernel ↔ netd only) | yes |

Stratum's 124–127 repurpose the legacy 9P2000 `Tstat`/`Twstat` numbers, which
the `.L` dialect leaves unused.

## Why this one has a document instead of an assertion

No compiler sees both trees. A `_Static_assert` can pin a number against a
literal in the same file — the discipline every other registry here runs on —
but nothing in either build can observe that Stratum's `STM_9P_TFADVISE` and
Thylacine's `P9_TWEFT` were once the same byte. The allocation authority has
to be a document because the constraint is cross-project.

`docs/9P-EXTENSIONS.md` is that document, and it carries what makes a
registry work rather than merely exist:

- **one allocation authority**, named as such — a new pair is taken there
  first and the enums follow;
- **a burn rule**: a number is spent once it has been on a wire in a release
  or a landed commit. Renumbering is a wire-ABI change requiring signoff, and
  is possible *only* while both endpoints are in-tree and nothing persists
  the number;
- **a domain column** recording which client↔server pairs actually carry each
  op — with the explicit warning that a collision across disjoint domains is
  still forbidden, because disjointness is an accident of today's wiring, not
  a guarantee;
- **a next-free pointer** (146/147), so the next allocator does not have to
  derive it;
- **a history** of the one time this went wrong.

And it is **backlinked from every allocation site in both projects**:
`9p_wire.h` twice, `ninep.rs` once, and Stratum's own `9p.h` — which points
across the project boundary at a Thylacine document, the link that makes the
authority real rather than aspirational.

## #371 — why a local enum cannot show you a shared space

The Weft quartet was born on 134–137, allocated by an author who read
`9p_wire.h`, saw the mirrored Stratum block end at `Tfallocate` 133, and
took the next free pair. Stratum also assigns `Tfadvise` 134/135, `Tpin`
136/137 and `Tunpin` 138/139.

The mistake was not carelessness. `9p_wire.h` mirrors the Stratum ops
**Thylacine issues** and not the ones it does not, so its highest number is a
function of Thylacine's usage, not of the space's occupancy — and it looks
exactly as authoritative either way. A local enum in a shared number space
cannot show you the space. That is the durable lesson, and it is why the
resolution was a registry rather than a bigger comment.

The collision was latent: Weft ops go kernel→netd only, `Tfadvise`/`Tpin` go
to stratumd only, and no session carries both. It was fixed anyway — one
number meaning two ops depending on which server answers is a standing hazard
for any future proxy or multiplexed session, and the disjointness that made
it safe is not a property anyone had promised to preserve. Both Weft
endpoints were in-tree with nothing persisting the number, so the quartet
moved to 142–145; Stratum's 134–139 are shipped ABI and stayed. POUNCE had
already taken 140/141 as the first pair free in *both* registries.

## Verification

Checked against all three enums, this batch:

- Thylacine `9p_wire.h`: 124–133 mirrored, 140/141, 142/143, 144/145 — and
  nothing at 146 or above;
- `libthyla-rs/src/ninep.rs`: 142–145 only, which is correct — netd serves
  the Weft ops and never the Stratum ones, exactly as the domain column says;
- Stratum `include/stratum/9p.h`: 124–139 plus 140/141.

**The registry is accurate on every allocation.** It is the one registry in
this pass that verifies clean, and the only one held by a document rather
than by per-copy assertions.

Its bookkeeping has drifted slightly in the harmless direction: the
"Defined in" column marks 128/130/132 as mirrored in `9p_wire.h` but not
124/126, which are also there. The column that must be right — the number,
the owner, the domain — is right; the descriptive column is stale. Worth
fixing on the next touch, and worth noticing that the rules protect the
load-bearing half specifically.

One grep hazard: `P9_WGA_BODY_LEN` is `153`, a byte length, not a message
type. A naive scan for numbers above 145 finds it.

## Change protocol

**Allocate in `docs/9P-EXTENSIONS.md` first, in the same change that adds the
enum entries.** Take the next free pair, extend the table, set the new
next-free pointer, and add the backlink comment at each definition site. A
pair that has been on a wire is burned; do not reuse it for a different op
even after the original is retired.

Adding an op to only one project's enum is how #371 happened, and the
registry says so in its own rules.

## The wire shape, for reference

Frame: `[u32 size][u8 type][u16 tag][body]`, size inclusive of the header.
`P9_HDR_LEN` is 7. `tag` may be `P9_NOTAG` only for `Tversion`. Strings are
9P-strings (`u16` length + bytes). A qid is **13 bytes on the wire** — `u8`
type, `u32` version, `u64` path — while `struct p9_qid` is 16 in memory; the
two are deliberately distinct, and the header says so. Walks are capped at
`P9_MAX_WALK` (16) components and `P9_NAME_MAX` (255) bytes per name.

The in-memory decode structs are **not** this ABI, with the exception of the
three that Loom promoted by copying them into a userspace buffer — see
[[abi-loom-ring]].

## Referenced by

[[sub-kernel-ninep-wire]] · [[sub-kernel-ninep-client]] ·
[[sub-kernel-ninep-session]] · [[abi-loom-ring]] · [[moc-boundary]].
