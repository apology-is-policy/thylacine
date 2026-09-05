---
id: abi-loom-ring
type: abi
kind: struct
stability: frozen
title: "The Loom ring ABI — five shared-memory structures, and three promoted into ABI by being copied out"
pinned-by:
  - "kernel/include/thylacine/loom.h: _Static_assert on all five sizes; offsets on sqe(2) cqe(2) hdr(1) params(1) buf_reg(1)"
  - "kernel/loom.c: _Static_assert on p9_attr / p9_setattr / p9_statfs sizes + 12 offsets"
mirrors:
  - "usr/lib/libthyla-rs/src/loom.rs: Sqe / Cqe / BufReg / Params (size + full offset_of! sets)"
  - "usr/lib/libthyla-rs/src/loom.rs: the ring header as five bare offset constants (HDR_SQ_HEAD=0 .. HDR_FLAGS=32), no struct, no assert"
created: 2026-08-02
updated: 2026-08-02
---
## The surface

Loom is the io_uring inversion: userspace posts 9P-shaped op descriptors into
a submission ring in a shared Burrow, the kernel's elected-reader 9P client
drives them, and R-messages return as completion entries. The whole
transport is **five structures on a page both sides write**, so the layout is
not a convenience — it is the protocol.

Unlike [[abi-t-stat]], this ABI has exactly **one** mirror, `libthyla-rs`.
Neither pouch nor the Go fork speaks Loom; the ring is native-only.

| struct | size | role | writer |
|---|---|---|---|
| `loom_sqe` | 64 | one submission entry (a native 9P op descriptor) | user |
| `loom_cqe` | 16 | one completion entry | kernel |
| `loom_ring_hdr` | 64 | the four head/tail words + flags | **both** |
| `loom_params` | 88 | the `SYS_LOOM_SETUP` in/out geometry | kernel (out) |
| `loom_buf_reg` | 16 | one registered-buffer descriptor | user |

The SQE is one cache line, deliberately, and carries a four-word reserved
tail (`_resv1[4]`) that later sub-chunks carved opcode-specific fields out of
without an ABI break — the buffer sub-offset at `_resv1[0]`, the second fid
for the two-fid mutation ops at `_resv1[3]`. The field is opcode-tagged:
`MKNOD` reuses `_resv1[3]` for its device minor. That is the reserved-tail
pattern working as designed, and it is also why the offset assertions matter
more here than the size ones — the size has not moved and will not.

## The ring header, and the inverse of the pin you would expect

Ownership is strict single-producer/single-consumer per word:

```
sq_tail  user writes   (produces)     kernel reads
sq_head  kernel writes (consumes)     user reads
cq_tail  kernel writes (posts)        user reads
cq_head  user writes   (reaps)        kernel reads
```

Each side advances only its own word, so there is no torn cross-write; the
release/acquire pairing across those four words is the wait/wake machine
[[spec-loom]] models, and it is where a ring TOCTOU or a CQ back-pressure bug
would live.

It is therefore the structure whose layout is *most* load-bearing — a
field mix-up here is not a crash but a silent corruption of the completion
protocol — and it is the **least pinned of the five**:

- kernel: the size, plus a single offset assertion (`sq_head at 0`) covering
  one of twelve fields;
- Rust: no `#[repr(C)]` struct at all. Five bare constants —

  ```rust
  const HDR_SQ_HEAD: usize = 0;
  const HDR_SQ_TAIL: usize = 4;
  const HDR_CQ_HEAD: usize = 16;
  const HDR_CQ_TAIL: usize = 20;
  const HDR_FLAGS:   usize = 32;
  ```

  read through an `AtomicU32` view. All five are correct against the kernel
  today. Nothing checks them, because there is no struct for `offset_of!` to
  measure.

The contrast within one file is the point: `Sqe` — which only userspace
writes — carries **eight** `offset_of!` assertions on the Rust side and two
on the kernel's. `Cqe` has three and two. `Params` has four and one.
`BufReg` has two and one. The header, which both sides write concurrently,
has one and zero. The pinning tracks *how easy the struct was to assert*, not
*how much a mistake would cost*. Tracked as task #44.

Those Rust `offset_of!` sets exist because a Loom-6d audit found them
missing and asked for them. The finding's reasoning was general — a
same-size reorder leaves `sizeof` unchanged and silently shifts a byte-pinned
ABI — and it stopped at the four structs that had a Rust type. The header
had none, so it was not measured; [[abi-t-stat]]'s Rust mirror was out of
scope, so it was not either.

## Three structures promoted into ABI by being copied out

`struct p9_attr` (160 bytes), `struct p9_setattr` (56) and `struct p9_statfs`
(64) are declared in `kernel/include/thylacine/9p_wire.h` as **decode
targets** — the in-memory shape a 9P2000.L `Rgetattr` / `Rstatfs` reply is
parsed into, distinct from the wire encoding (the embedded `struct p9_qid` is
16 bytes in memory and 13 on the wire, and the header says so).

The `LOOM_OP_GETATTR` / `STATFS` / `SETATTR` opcodes changed that. The kernel
copies the parsed record **verbatim** into the caller's registered buffer, so
the in-memory layout became the userspace-visible output layout. Twelve
assertions pin it.

They live in `kernel/loom.c`, labelled *"Loom GETATTR output ABI"*. The
guard is effective — `loom.c` includes `9p_wire.h`, so a field add to
`p9_attr` does trip the build — but the *definition site carries no marker
at all*. A developer extending `p9_attr` with a new 9P2000.L attribute reads
a plain internal decode struct, and gets a build failure citing Loom, from a
subsystem they had no reason to be thinking about. The pin is in the
consumer; the surprise is in the definition.

**The claimed native mirror does not exist.** Both `loom.h` and `loom.c` say
the layout is one *"the native `libthyla_rs::loom` side mirrors at Loom-6d"*.
Loom-6d landed and the arc is complete. There is no `P9Attr`, no `Statfs`, no
`SetAttr` type anywhere in `libthyla-rs`. `op::GETATTR` and `op::STATFS`
exist as opcode constants, so a native program can submit one — and then
receives 160 bytes it has no declared type to decode, and must hand-read by
offset. Twelve assertions pin an output ABI with zero declared consumers on
the side they were written for. Tracked as task #45.

## Change protocol

**Frozen.** A layout change to any of the five ring structures is an ABI
break, not an append — the ring is mapped, live, and read concurrently from
both sides; there is no version negotiation and no room to grow a structure
whose size is baked into the geometry the kernel reports at setup.

Growth happens in the reserved tails instead: `loom_sqe._resv1[4]` and
`loom_params._resv1[4]`, both already sized, both consumed opcode-by-opcode.
A new opcode claims tail words and documents them in the per-opcode field map
in `loom.h`; nothing about the layout moves.

The three promoted `p9_*` records are **append-only at the tail** and their
size assertions must be updated in the same commit, since the assertion is
what tells the author their edit reached a userspace ABI at all.

## Prosecution

- A same-size reorder inside `loom_ring_hdr` passes every assertion on both
  sides and silently swaps two words of the completion protocol. The Rust
  offsets are literals; nothing measures them.
- A field appended to `p9_attr` trips `loom.c`'s size assertion — which is
  the guard working, but the message names Loom and the edit was in
  `9p_wire.h`. Read it as *"this struct is a userspace ABI"*, not as a Loom
  problem.
- `LOOM_SQE_FID2` and `LOOM_SQE_BUF_OFF` are the same reserved word for
  different opcodes. A new opcode claiming `_resv1[3]` for a scalar must not
  also be a two-fid op.
- The registered-buffer bounds (`buf_off + len <= registered length`) are
  checked against the **kernel's snapshot** of the SQE, never re-read from
  the shared ring — the submit-time-pin discipline. A future op that re-reads
  a descriptor field after the check reopens the TOCTOU
  [[spec-loom]]'s buggy configurations exist to forbid.

## Referenced by

[[sub-kernel-loom]] · [[spec-loom]] · [[abi-t-stat]] · [[abi-ninep-wire]] ·
[[moc-boundary]].
