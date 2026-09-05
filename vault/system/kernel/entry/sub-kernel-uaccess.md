---
id: sub-kernel-uaccess
type: sub
parent: moc-kernel-entry
title: "uaccess — touching a user address from the kernel without dying"
code:
  - arch/arm64/uaccess.S
  - arch/arm64/uaccess.c
  - arch/arm64/uaccess.h
audit: hard
guarded-by: [inv-i13]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/reference/40-uaccess.md"
  - "docs/CONCURRENT-FS.md CF-3"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Kernel code frequently needs to read or write a userspace address: a syscall
staging a buffer, a futex comparing a word, a thread exit publishing a zero to
a joiner. The page may be in a mapping but not yet installed in the page
tables, so the access faults — and a kernel-mode fault is normally fatal.

This is the machinery that makes those specific accesses recoverable, and only
those.

## Contract

A small set of primitives — load and store a byte, load and store a word, copy
a buffer in either direction — each returning zero or `-1`. A `-1` means the
access faulted and the caller must treat it as a bad-address error. The kernel
does not extinct.

Callers validate the address range before calling; the primitives do not
range-check. The word primitives additionally require alignment from the
caller, because an alignment fault is *not* in the recoverable set.

## Mechanism

### The faulting instruction is a designated one

Each primitive contains one or more instructions marked as the fault point. For
every marked instruction the assembler emits a table entry pairing it with a
label that returns `-1`.

The table lives in read-only data and stores **relative** offsets rather than
addresses, so it needs no relocations and is unaffected by where the kernel is
loaded — the same encoding, for the same reasons, that the crash dump's symbol
table uses.

### One fault, two possible outcomes

When the kernel takes a synchronous fault, the handler asks three questions in
order: did this come from the kernel, is the faulting address in the user half,
and is the faulting instruction in the table. Only if all three hold is it a
deliberate crossing. Then:

- **If a mapping covers the page**, the kernel installs it and returns without
  touching anything. The `eret` re-executes the faulting instruction, which now
  succeeds. The caller never learns anything happened.
- **If it does not** — no mapping, permission denied, or out of memory — the
  handler overwrites the saved return address with the fixup label. The `eret`
  lands there instead, the primitive returns `-1`, and the caller reports a bad
  address.

The second is the unusual move and worth stating plainly: **the fault handler
redirects control flow by rewriting the return address.** The primitive never
learns why it failed; it just finds itself at a label that returns `-1`.

### No writeback, and that is the whole retry argument

The retry path re-executes an instruction that already faulted once. That is
only correct if the instruction left nothing behind — so the faulting loads and
stores use no writeback addressing mode, and the pointer advance is a separate
instruction placed *after* the fault point. On resume the re-executed access
sees exactly the registers it saw the first time.

This is easy to break by writing what looks like tighter code. A
post-increment addressing mode on a fault point would advance the pointer, fault,
and then re-execute against the *advanced* pointer — skipping a byte on every
demand-paged page boundary, silently, in a copy path.

### The bulk copies are three fault points wearing one coat

A byte-at-a-time loop through a per-call primitive costs a function call per
byte. The bulk copies replace that with a byte head that aligns the user
address, an eight-byte body, and a byte tail — so three marked instructions per
direction, all sharing one fixup label. The kernel side of the copy may be
unaligned, which is fine because alignment checking is off for normal memory.

A fault partway through leaves the bytes before it already transferred. That
partial-copy property is documented and unchanged from the per-byte era; the
caller returns its error and the buffer must not be trusted.

## Data structures

The fixup table: pairs of signed 32-bit relative offsets, one pair per marked
instruction, in a dedicated read-only section with linker-provided bounds. A
compile-time assertion pins the entry size, and another pins the user-address
bound against the memory layer's copy of the same constant — so a drift that
would let a kernel fault slip past this check fails the build.

## Concurrency

None owned. The table is immutable read-only data built at link time. The
lookup takes no locks, allocates nothing, and touches no shared mutable state,
which is what lets it run from inside a fault handler.

The demand-page call it makes on the success path takes the faulting Proc's
mapping lock; that is the memory layer's discipline, not this one's.

## Invariants enforced

**[[inv-i13]]** — this is the invariant's crossing half. The kernel/user split
means kernel code cannot casually touch a user address; this machinery defines
the narrow, enumerated set of places where it may, and guarantees that each of
them fails closed. Every other kernel-mode fault on a user address remains what
it should be: a symptom of a corrupted pointer, and fatal.

The narrowness is the security property. The check requires a kernel-mode fault
*and* a user-half address *and* a table hit — so a corrupted kernel pointer that
happens to land in the user half still extincts, because the faulting
instruction is not in the table.

## Error paths

`-1` from every primitive on a fault. Zero from the table lookup for an
instruction that is not a designated crossing, which the handler reads as "not a
uaccess fault" and passes on to the ordinary fatal path.

There is no error path for a bad caller: an unvalidated address or an unaligned
word access is a caller bug, and the alignment case is not recoverable.

## Performance

The primitives are straight-line. The bulk copies move eight bytes per
iteration with no call overhead, which is the point of their existing — they
replaced per-byte loops that cost a function call per byte and capped bulk I/O
throughput.

The lookup is a **linear scan** of the table, run once per kernel-mode
user-address fault. That is once per page of a bulk copy into a lazily-mapped
buffer, over a table of ten entries — genuinely negligible today, and worth
noticing only because the table grows by design and its scan is on the demand
paging path.

## Prosecution

- **A new fault point needs a table entry.** The entry is what separates
  "recoverable crossing" from "kernel pointer corruption"; without it the
  access is simply fatal.
- **Fault points must not use writeback addressing.** The retry re-executes
  them.
- **The pointer advance must stay after the fault point**, for the same reason.
- **The user-half bound must stay pinned to the memory layer's.** The assertion
  is the only thing keeping the two from drifting apart.
- **The check must stay a conjunction.** Dropping any of the three conditions —
  kernel-mode, user-half, in-table — turns real memory corruption into a
  silently absorbed `-1`.
- **Callers must validate range and alignment.** The primitives do not, by
  design, and an alignment fault is not recoverable here.

## Seams

- **The lookup is linear.** Fine at ten entries; the growth is unbounded by
  design ("added on demand"), and nothing flags when a scan stops being
  negligible.
- **Alignment faults are outside the recoverable set**, so an unaligned kernel
  access to a user address extincts rather than returning `-1`. Callers that
  could produce one carry their own alignment gate — which makes those gates
  load-bearing in a way their call sites do not always say.

## Caveats

- **Three separate comments claim this file provides one primitive.** The header
  says "at v1.0 only `uaccess_load_u8` is provided" directly above declarations
  for six; the assembly's design note says "we export a single primitive"; the
  lookup's comment says "at v1.0 the table has one entry". There are ten fault
  points and six primitives. Nothing is wrong with the *code* — every primitive
  is correct and every entry is present — but the summarizing prose describes
  the first version of a file that has since grown five times, and a reader who
  trusts it will mis-size both the table and the surface. Same pattern as the
  console's header block and the reference document's vector table: current
  comments beside the code, stale ones at the top.
- **The public-surface list in the header enumerates two of six.** It is the
  same drift, in the one place a caller is most likely to look first.

## Provenance

[[chg-2026-08-02-entry-sweep]].
