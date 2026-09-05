---
id: abi-t-stat
type: abi
kind: struct
stability: append-only
title: "struct t_stat — the file-metadata record, and its seven mirrors"
pinned-by:
  - "kernel/include/thylacine/syscall.h: _Static_assert on size + all 14 field offsets"
mirrors:
  - "usr/lib/libt/include/thyla/syscall.h: struct t_stat (size + 8 of 14 offsets)"
  - "usr/lib/libthyla-rs/src/fs/metadata.rs: #[repr(C)] Metadata (size only)"
  - "usr/lib/pouch/patches/0010-pouch-fstat-lseek.patch: struct t_stat in fstat.c (size only)"
  - "usr/lib/pouch/patches/0019-pouch-stat.patch: struct t_stat in fstatat.c (size only)"
  - "usr/lib/pouch/patches/0021-pouch-pty.patch: struct pouch_tstat in ioctl.c (size only)"
  - "usr/lib/pouch/patches/0024-pouch-fs-process-wires.patch: unsigned char t[88] + literal +40 in faccessat.c (NO guard)"
  - "go-thylacine src/syscall/syscall_thylacine.go: type Stat_t (NO guard)"
created: 2026-08-02
updated: 2026-08-02
---
## The layout

The result of `SYS_FSTAT` and `SYS_STAT`. **88 bytes**, naturally aligned,
Plan-9-shaped at the core and POSIX-shaped at the surface — the qid carries
the 9P identity verbatim, and `mode`/`size`/the timestamps are POSIX so the
pouch boundary-line can fill musl's `struct stat` without an intermediate.

| off | type | field | note |
|---|---|---|---|
| 0 | u64 | `size` | bytes |
| 8 | u64 | `qid_path` | 9P qid.path — inode-ish, unique **within a Dev** |
| 16 | u64 | `atime_sec` | epoch seconds; **0** at v1.0 |
| 24 | u64 | `mtime_sec` | 0 at v1.0 |
| 32 | u64 | `ctime_sec` | 0 at v1.0 |
| 40 | u32 | `mode` | POSIX mode bits (`T_S_IF*` \| rwx) |
| 44 | u32 | `nlink` | 1 for regular files at v1.0 |
| 48 | u32 | `qid_vers` | 9P qid.vers |
| 52 | u8 | `qid_type` | 9P qid.type (`P9_QT*`) |
| 53 | u8[3] | `_pad_qid` | |
| 56 | u32 | `blksize` | I/O size hint |
| 60 | u32 | `_pad_blksize` | |
| 64 | u64 | `blocks` | 512-byte blocks |
| 72 | u32 | `uid` | A-2a owner principal-id |
| 76 | u32 | `gid` | A-2a owning group |
| 80 | u32 | `devno` | #100 per-instance device number |
| 84 | u32 | `_pad_dev` | to 8-byte alignment |

It grew twice: **72 → 80** (A-2a appended `uid` + `gid`, the durable owner
the kernel rwx layer reads) and **80 → 88** (#100 appended `devno`).

`devno` is the field that makes the record self-sufficient. A `qid_path` is
unique only within one Dev, and a login session mounts several
independently-inode-numbered Stratum datasets — the system pool and a
per-user home. File identity is therefore the **pair** `(devno, qid_path)`,
which is exactly what a static single-instance Dev satisfies by reporting
`devno` 0 over its own self-consistent qid space, and what `dev9p` satisfies
by minting one per attach session. Go's `sameFile` and gopls's
`robustio.getFileID` both key on the pair.

## The mirror set

Eight declarations of this layout exist: the kernel's, and **seven mirrors**.

| site | shape | size guard | offset guards |
|---|---|---|---|
| `kernel/include/thylacine/syscall.h` | `struct t_stat` | yes | **14 — every field** |
| `usr/lib/libt/.../syscall.h` | `struct t_stat` | yes | 8 of 14 |
| `libthyla-rs/src/fs/metadata.rs` | `#[repr(C)] Metadata` | yes | **none** |
| pouch `0010` fstat.c | `struct t_stat` | yes | none |
| pouch `0019` fstatat.c | `struct t_stat` | yes | none |
| pouch `0021` ioctl.c | `struct pouch_tstat` | yes | none |
| pouch `0024` faccessat.c | `unsigned char t[88]` | **none** | none |
| go fork `syscall_thylacine.go` | `type Stat_t` | **none** | none |

gopls's `robustio_thylacine.go` is a **consumer** of the Go mirror, not an
eighth layout — it reads `Stat_t.Dev` and `.QidPath` by name, so it breaks
only if the Go mirror is already wrong.

Two of the seven carry no layout guard at all. The Go `Stat_t` has none
because Go has no `_Static_assert` and none was hand-rolled from
`unsafe.Sizeof`. Pouch's `faccessat` is more interesting: it declares no
struct, reading the mode as

```c
unsigned char t[88];
...
unsigned mode = (unsigned)t[40] | ((unsigned)t[41] << 8) ...
```

— a literal size and a literal offset. It only needs `st_mode`, and
open-coding the one field it wants is defensible; what it costs is that the
`88` and the `40` are invisible to every tool and every grep for `t_stat`.

## What the guards actually catch

**Each mirror's `_Static_assert(sizeof(...) == 88)` compares that mirror
against a literal the same author typed into the same file.** It is a check
that the struct is what the comment beside it says — never that it is what
the kernel says.

Trace a kernel growth to 96 bytes. Exactly **one** assertion fires: the
kernel's own, in the file already being edited. The other six size asserts
still read 88, and are still true of their own 88-byte structs, so they pass.
The two unguarded mirrors pass by having nothing to check. The build is
green, and the kernel writes 96 bytes into every one of seven 88-byte
buffers.

That is the #100 failure exactly, and it is worth being precise that the
guard set is *unchanged* since. The kernel's assertion message names the
obligation in full — *"EVERY mirror (libt, libthyla-rs, pouch patch 0010,
the go-thylacine syscall.Stat_t) MUST grow in lockstep"* — and that sentence
is the entire enforcement. It is a note to the author, delivered at the
moment they are already editing the line, about six other files. Useful; not
a check.

The historical detection was a crash. Pouch's `0019` and `0021` mirrors were
left at 80 through the #100 append and were found by a boot segv in
`pouch-hello` plus a manual `struct t_stat` grep — not by the build, which
stayed green the whole time.

## The offset half, and an asymmetry worth naming

A same-size field reorder — swapping two `u32`s — leaves every `sizeof`
assertion true and silently shifts the record. Only offsets catch that. The
kernel pins all 14; `libt` pins 8, omitting `atime_sec`, `mtime_sec`,
`ctime_sec`, `nlink`, `qid_vers`, and `blksize`; the other five mirrors pin
none.

The asymmetry is with [[abi-loom-ring]]. That ABI's Rust mirror carries
`offset_of!` assertions on every field of every struct — added deliberately,
as the fix for a Loom-6d audit finding whose reasoning was general: *"a
same-size field reorder leaves sizeof unchanged but silently shifts the
byte-pinned ABI the native mirror reads."* The argument is identical here,
and here it applies to seven mirrors instead of one. It was applied to the
struct the audit was scoped to, not to the class the finding described.

What would actually close this is single-sourcing: one header the C mirrors
include, a generated Rust/Go layout, or a boot probe that compares the
kernel's `sizeof` against each mirror's. None exists. Tracked as task #43.

## Two places the prose has drifted from the struct beside it

- `libthyla-rs/src/fs/metadata.rs` opens with *"Backed by `struct t_stat`
  (80 bytes, ABI-pinned)"*. The struct 30 lines below is documented as 88 and
  asserts 88. The stale line is the module header — the first thing read.
- pouch `0021` introduces its mirror as *"Mirror of the kernel struct t_stat
  (80 bytes, layout pinned by kernel `_Static_assert`s)"*, immediately above
  an 88-byte struct with an 88-byte assertion. Both halves of that sentence
  are wrong: the size, and the claim that the kernel's assertions pin *this*
  copy. Pouch `0019` repeats the second half — *"offsets pinned by the
  kernel's `_Static_assert`s"* — which is precisely the belief that makes the
  mirror set look safe.

The `0021` mirror also renames the struct to `pouch_tstat`, so a
`struct t_stat` grep — the tool that found the #100 stragglers — misses it.

## Change protocol

**Append-only at the tail; never reorder, never renumber a field's offset.**
A field add extends the record and *every* mirror rebuilds in one commit.
There is no persistent on-disk consumer, so growth is cheap — the cost is
entirely in the mirror count.

The obligation list is the `mirrors:` header above, which is the count this
note asserts: **seven**. `CLAUDE.md`'s #100 addendum names six, missing pouch
`0024` (which landed later) and counting gopls (a consumer). A change worked
from that list is complete by its own accounting and short by one file.

## Prosecution

- A mirror left at the old size is overrun by the kernel's copy-out — a
  stack smash in the *caller's* frame, surfacing far from the struct.
- A same-size reorder passes every existing guard except the kernel's own
  offset set. Five mirrors have no offset guard.
- `faccessat`'s `t[88]` + `+40` is invisible to a `t_stat` grep; so is
  `pouch_tstat`. Sweep by *size literal* as well as by name.
- A new pouch patch that needs one field is the recurring temptation to add
  an eighth copy. Patch `0031` faced it and refused — reusing `faccessat`'s
  probe rather than repeating it, with the reasoning recorded in-line: *"a
  duplicated mirror is only ever verified against ITSELF."* That is the
  correct instinct and the standing precedent.

## Referenced by

[[sub-pouch-fs]] · [[sub-kernel-ninep-dev9p]] · [[abi-loom-ring]] ·
[[moc-boundary]].
