---
id: sub-kernel-syscall-dispatch
type: sub
parent: moc-kernel-entry
title: "The syscall dispatcher — argument marshalling, the staging tiers, and where a gate is allowed to live"
code:
  - kernel/syscall.c
audit: hard
guarded-by: [inv-i13, inv-i32, inv-i22, inv-i27, inv-i34, inv-i43, inv-i44]
validated-by: [prose, gate-smp, gate-interactive]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 13"
  - "docs/VIVARIUM.md"
  - "docs/LINEAGE.md"
created: 2026-08-03
updated: 2026-08-18
---
## Purpose

The single chokepoint where untrusted register values become typed C arguments.
[[sub-kernel-syscall-abi]] declares what crosses; this is the machinery that
takes it across — the dispatch switch, the user-pointer validator, the staging
buffers, and the split between the layer that talks to userspace and the layer
that does the work.

**What this dossier covers, precisely.** The file is 11138 lines and holds all
103 handlers. Most of a handler is *policy belonging to its own subsystem* — a
Burrow handler is described by [[sub-kernel-burrow]], a pts handler by
[[sub-kernel-pts]], and so on across roughly thirty dossiers. The subject here
is what surrounds them: dispatch, marshalling, validation, staging, error
convention, and the layering rule that decides where an authority check may sit.
A handler's own semantics are its subsystem's; a handler's *shape* is this
dossier's.

## Contract

`syscall_dispatch` receives the interrupted register frame. Since the phenotype
prologue landed it no longer starts by reading `x8`: it first resolves the
calling process and, if that process declared a Linux phenotype, hands the whole
frame to the translation layer, which may answer the call outright or **rewrite
the number and the argument registers in place** before the native switch sees
them. Only then is `x8` read and switched on.

Most arms then behave as before: read arguments from `x0..x5`, call a handler,
write the result to `x0`, return. Three arms never return — the two exit calls
and the thread-exit call, which abandon the frame on the exiting thread's kernel
stack until the reaper collects it. Three others take **the frame itself** and
so break the uniform result-write; see below.

An unknown number writes `-1` and returns.

**The dispatcher is no longer contentless.** The pre-phenotype claim — no
validation, no capability check, no bookkeeping, every gate a handler's — held
exactly and is now wrong in one precise way: the prologue does real work for a
phenotyped process, including one side effect (a socket-table drop) that must
happen before a native handler runs. It still holds no *authority* check, which
is the part [[inv-i43]] cares about: the prologue changes what number is being
called, never what the caller is allowed to do.

## Mechanism

### The dispatch is exactly as wide as the ABI says

103 syscall numbers are live and the switch has exactly 103 distinct arms;
neither set has a member the other lacks. (A grep for case labels finds 106 —
three of them belong to a second, inner switch that routes the three pts control
calls to their tty backends, not to the dispatcher.) Almost every arm reads only
`x0` through `x5` and `x8`, the argument window the ABI declares; the three
frame-taking arms below are the stated exceptions. Three syscalls use the sixth
argument; most use one or two.

**No arm falls through to its neighbour, and here that is enforced rather than
observed.** The three arms with no `return` are the never-returning ones, and
each of their handlers carries `__attribute__((noreturn))` — so a handler that
gained a return path would be a compile error at the handler, not a silent
fallthrough into the next syscall's arm. Each also ends in an `extinction()`
backstop for the case where the noreturn primitive beneath it returns anyway.
The width claim itself is still a measurement rather than a mechanism.

### The phenotype prologue: what a Linux-shaped call meets before the switch

A process declares a phenotype at spawn. `PHENO_NATIVE` is the default and
every process outside a declared vivarium; the branch is one predictable test on
an already-hot cache line and the native path is byte-unchanged. A
`PHENO_LINUX` process goes through `viv_linux_dispatch`, which lives in this
file (not in the vivarium module) precisely because it is dispatch. It does four
distinguishable things:

- **Intercepts the one call that rewrites the frame.** Linux's `rt_sigreturn` is
  the phenotyped spelling of the note-return call, and it is handled by direct
  interception rather than by a translation row, because both of the other
  shapes are wrong for it: a renumber would copy six argument words when the
  Linux call takes none, so a garbage sub-command would arrive where a literal
  zero is required; and a tier-2 shell returns a value the caller stores into
  `x0`, which would immediately overwrite the `x0` the note restore just put
  back. The interception *is* the implementation.
- **Runs one entry hook with a side effect.** A phenotyped `close` drops the
  process's socket-table entry for that descriptor *before* the native close
  runs, unconditionally, because the descriptor index is freed by the native
  path and reused — so a surviving `(proto, N)` entry would later be found by an
  unrelated file's operation and a dial verb written to a stranger's connection.
  The hook is deliberately not a translation row: `close` must stay a plain
  renumber that falls through, so descriptor teardown keeps exactly one
  implementation.
- **Translates in place and falls through.** The common case rewrites `x8` and
  the six argument registers from the translation result and returns *true*,
  meaning the native switch runs — on registers that are no longer the ones
  userspace set.
- **Names four different kinds of "no".** A tier-2 row that declined these
  arguments, a number with no translator, a number declined by recorded policy,
  and a number absent from the table are all reported separately even though all
  four return the same errno. The distinction is the point: "widen this domain"
  and "write this translator" are different jobs, and hiding the considered
  decline behind the never-considered one would make a guest failure unreadable.

The `true`/`false` convention is worth stating because it inverts the intuitive
reading: **false means handled** (the frame is final, write no result), true
means fall through to the native switch.

Nothing pins the translation table's native targets to real dispatch arms by
construction — but a compile-time assertion pins the table's native/Linux
boundary constant to the highest assigned native number, and a census of every
native symbol the table can emit finds a dispatch arm for each. A translated
number with no arm would land on `default` and return the bare sentinel with no
unserved report, which is the one failure shape the four-way naming above does
not cover.

### Three handlers take the frame, and the reason is a category

Most handlers take register values. Three take `struct exception_context *`
itself, and they share a property: **the frame is the subject of the call, not
the means of returning from it.**

- **execve** rewrites it — zeroing every general-purpose register and repointing
  the exception-return address and stack at the new image, so the syscall's own
  return *is* the transition into the new program. There is no separate entry
  path and no window where the new address space is live while the program
  counter still points into the old one.
- **rfork** copies it, so a second thread can return onto it. The mirror image:
  the child's `x0` is set to zero in *its* copy and the parent's store never
  touches it, because the child is a different thread on a different stack that
  never returns through the dispatcher at all.
- **noted** restores it from a saved pre-handler snapshot.

This forces the dispatcher to abandon the uniform "write the result to `x0`"
rule for exactly one arm: execve's result is stored **only if non-zero**,
because on success the handler has already zeroed `x0` on purpose and storing a
return value would hand a fresh program a register it never asked for. rfork's
store is unconditional and safe for the reason above; noted's arm stores nothing.

### Two layers, and the rule about which one may hold a gate

Forty-nine syscalls are split in two. A `_handler` takes raw register values,
resolves the current thread, validates user pointers, stages buffers, and calls
a `_for_proc` inner that takes an explicit process and kernel-side buffers.
Forty-one of those inners are non-static, and that is what makes them the
testable half — most are called from the kernel's own test suite, and several
from production kernel code that needs the operation without a syscall frame.

The layering rule follows from that: **the handler may own only what is about
userspace.** Thread resolution, pointer validation, staging, register
narrowing. Everything else — and every authority gate in particular — belongs in
the inner, because an inner is separately callable and a gate above it is a gate
some caller does not pass.

The rule holds nearly everywhere. Where a syscall has no inner, the gate is in
the handler by default and nothing can bypass it. Where a syscall has an inner,
the gates are in the inner. One exception is in Caveats.

### A third shape: one core, two front ends differing in argument *shape*

execve and rfork are split a different way, and the difference is worth naming
because the two splits answer different questions. The handler/inner split
separates *userspace concerns* from *the work*. The core split separates *the
argument shape* from *the decision* — and it exists because the same syscall
arrives in two incompatible register layouts:

- **execve.** The native ABI passes an argument blob already concatenated in
  user memory; the Linux ABI passes a pointer array that must be walked and
  repacked, and has no single user address to hand over. Both front ends produce
  kernel-side buffers and call one core.
- **rfork.** The native ABI's registers are flags/stack/tls. A Linux `clone`
  arrives in a different order entirely, and on the call that matters carries
  *garbage* in three of them — values the boundary library's `__clone` moved
  from registers the caller never set. So the phenotype shell must supply
  translated values rather than hand the core a raw frame. The core takes its
  three arguments explicitly and the native reader is a four-line register read.

The gate-placement rule generalises to this shape and the code says so
explicitly. In execve, the multi-thread gate sits in the core rather than in
each front end **so it cannot be forgotten by one** — at the cost of a wasted
argument copy on a refusing path, which is the right trade. The argument blob's
packing contract is validated in the core for the same reason and a sharper one:
the stack builder *extincts* on a NUL count that disagrees with the argument
count, so a mis-packed blob from either front end must become a clean `-EINVAL`
before it reaches the loader. Validating once, below both builders, turns a bug
in either front end into an error return instead of a dead kernel.

### execve's ordering is the file's most consequential, and it is ordered by what can still fail

The sequence is: copy both user-side vectors into kernel memory (after this the
user addresses mean something else entirely) → gate → resolve the program in
the **caller's** namespace, through the same helper every spawn uses so exec and
spawn cannot diverge on what is executable → build the whole new image in a
**detached** address space → commit → stamp identity → consume close-on-exec →
rewrite the frame.

Two placements in that list are load-bearing and each carries its reason in the
source:

- **Detached build.** Nothing touches the calling process until the commit, so a
  load failure leaves it running its old image untouched. On failure the
  detached space is the only reference and its release drains whatever got
  mapped.
- **Everything that closes or renames happens *after* the last thing that can
  fail.** The name and executable-path stamps land after the commit, because a
  process advertising a program it is not running is worse than one that is
  briefly unnamed. Close-on-exec runs after the commit for the same reason and a
  second one: those closes *sleep* (a descriptor's teardown sends a 9P clunk),
  which is legal at that point and would not be earlier. It runs *before* the
  frame rewrite so no instruction of the new image can observe a descriptor that
  was supposed to be gone.

The frontier this exposes: the native execve **preserves the environment** and
takes no envp argument at all. That is the ABI meaning rather than a shortcut —
the request is `execv`'s, "run this program, keep my environment" — and a caller
that wants to replace the environment writes `/env` first. The projection is
staged before the commit so a later failure cannot have disturbed the caller's
own environment.

### The capability is checked once, at mint, and the object type carries it after

The JIT surface is the clearest instance of the pattern that makes the layering
rule tractable. Emitting executable code requires a capability — but only the
create call checks it. Neither the publish call nor the teardown call re-checks
anything, and that is deliberate rather than an omission: publish requires its
range to lie inside a live VMA whose Burrow is of the code type, and only the
capability-gated create mints one. Teardown is explicitly reasoned about in the
source — releasing memory you already own is not an exercise of the emit
authority, and gating it would turn a capability expiry into a leak.

So the authority is enforced by *kernel-minted object type*, not by repeating a
capability check at every touch. A re-check would be the weaker design: it has
to be added to each new operation, and forgetting one is silent.

### The hardware-mint sequence, and where the same idea is factored and where it is copied

The three DMA-family create calls — plain, weave, and GPU buffer object — each
run the identical [[inv-i34]] sequence: resolve the thread, load the
hardware-create capability atomically, validate the rights word against the
full-rights mask, reject a zero size, ask the allowance whether it permits a
buffer of that size (**CreateBegin**), mint the object, then install the handle
through the allowance-aware allocator that re-checks revocation under the
allowance lock (**CreateCommit**), releasing the object if the install loses that
race.

The two-step create is the whole point: the lock-free permit check followed by
an install that re-checks under the lock is what closes the in-flight-create vs
device-removal race that would otherwise let a revoked driver keep a fresh
handle.

Which makes it worth recording *how* the three copies are related, because the
same three-subtype family is factored on one side of this boundary and
duplicated on the other. The object layer parameterises it — one body taking
size, envelope, and subtype, with three one-line wrappers. This file does not:
the three handlers are twenty-six-line copies differing in **exactly one token**,
the name of the mint function they call. See Caveats.

### Staging is two-tier, and the tiers exist for opposite reasons

Byte I/O bounces through kernel memory. Operations up to the 4 KiB stack scratch
use a stack buffer; larger ones take a transient heap allocation so a single
call can move up to the 128 KiB transfer maximum. The stack tier exists to make
the metadata-storm path free; the heap tier exists because the stack tier was
the bulk-read ceiling — two thirds of a compiler build's reads were exactly one
4 KiB chunk against a much larger negotiated message size.

The heap tier is user-drivable kernel memory held across a potentially
indefinite device operation, so it is budgeted per process. Charge precedes the
allocation, uncharge accompanies every free, and an over-budget request degrades
to the stack tier rather than failing — a short transfer is a correct answer.
The trusted boot chain is exempt.

The balance is worth stating because it reads wrong at a glance: the length
variable is *reassigned* to the stack bound when the heap tier is not taken, so
charge and uncharge appear to use different values. They cannot — the
reassignment happens only when no allocation is held, and every uncharge is
guarded on holding one. All four staging sites are structurally identical.

### The copy happens before the lock, on purpose

The console write stages its bytes into kernel memory *before* claiming the
console writer role. Faulting a user page can sleep, and holding the console
across an unbounded page-in would stall every other writer behind it. The same
shape appears wherever a handler stages then acts: validate, copy, then engage
the subsystem.

### Two error conventions, and the collision that forced the steering

Older syscalls return a bare `-1`. Newer ones return a negative errno. The
errno registry keeps its permission code at 1, so `-T_E_PERM` is bit-identical
to the bare sentinel — a deliberate collision, documented, which is why handlers
on the errno convention steer permission-shaped failures to the access code
instead.

Where a device's error is forwarded, an out-of-window negative is clamped to a
generic I/O error, so a device cannot punch a value through the boundary
library's error window and have it read as an enormous success.

## Data structures

None owned. The dispatcher operates on the exception frame
([[sub-kernel-exception]]) and on per-process state it does not define. Its own
state is the two staging buffers, both stack-local, and a per-process atomic
byte counter for the heap tier.

## Concurrency

Each syscall runs on its caller's own thread with its own stack frame, so the
staging buffers are private by construction. Three shared concerns:

- **The heap budget counter** is a per-process atomic updated by
  compare-and-swap. Sibling threads of one process contend on it; the loop is
  the standard bounded retry.
- **Handle lookups return a held reference.** The lookup helpers acquire the
  object's reference under the handle-table lock and transfer it to the caller,
  who must release it on every exit path. This is what makes a blocking device
  operation safe against a sibling thread closing the same descriptor
  mid-call — the pre-existing contract returned a bare pointer into the live
  table, which was exactly that race.
- **A reference is taken across a lock drop where the work is long.** The
  instruction-cache publish takes a Burrow reference under the address-space
  lock, drops the lock, does the maintenance, and releases — so a sibling
  tearing the region down concurrently frees it there instead of underneath.
- **A charge claim is taken before the drop that could free its record.** The
  detach path snapshots the Burrow pointer rather than the VMA — the unmap frees
  the VMA struct, so that pointer dangles the moment it returns — and claims the
  page charge *before* the drop, because a freeing drop takes the payment record
  with it.

### Who paid is recorded, not inferred

The detach path's refund used to be decided by shape: an eager anonymous region
refunds its recomputed occupancy. That is wrong in both directions once a region
can be shared into another process, and the correction is a good example of the
distinction this file has to keep straight.

The failure it fixed: when a region survives because it was **shared out**, the
detaching process has walked away from pages it can no longer reach. Charging it
caps it for nothing, and nothing downstream can settle the charge either — the
last drop is then the *consumer's* teardown, generic code in another process
holding that process's lock, with no way to name the payer. That is the shape the
network daemon hits on every closed zero-copy flow, and it leaked sixty-four
pages a flow.

The replacement is attribution: a nonzero claim means "this process is the
recorded payer for this region", which is strictly narrower than "this is an
eager anonymous region". A region that was never charged now refunds nothing,
enforced by attribution rather than by enumerating shapes. Two discriminations
are load-bearing and easy to collapse: **shared-out**, not "does anything else
still hold this" (the process's *own* other claim also keeps the region alive,
and there the charge must stay); and **claim before drop**, not after.

## Invariants enforced

**[[inv-i13]]** — the user-pointer validator is the boundary. It rejects null,
rejects anything at or above the user-VA top, and rejects a length that would
wrap or cross that top. Sixty-nine call sites use it. It validates a *range*,
not a pointer: a zero length passes unconditionally, which is correct because
nothing is dereferenced, and is why every caller pairs it with the length it
will actually touch.

**[[inv-i32]]** — the staging budget is this file's contribution to the
per-process resource floor, joined by two more axes since: the page charge's
payer attribution above, and the spawn-time page budget, which a child inherits
unless the parent holds the raise authority. That authority is itself a
spawn-permission bit taking the one-hop delegation shape — console-attached
**or** an existing holder — so a chain from init through login and the shell to a
build driver can carry it without any of them being console-attached. The bit
gates conferring the *authority*; the raise it authorises is still bounded by
the hard maximum.

**[[inv-i34]]** — the three hardware-mint handlers carry the CreateBegin /
CreateCommit pair described above. The allowance is what bounds them; this file
is where the two steps are written.

**[[inv-i43]]** — the phenotype prologue is where "shape, never authority" is
kept. It renumbers and remaps arguments; it reads the capability word nowhere and
writes it nowhere. A phenotyped process meets exactly the same gates in exactly
the same handlers as a native one.

**[[inv-i44]]** — execve's detached build and rfork's frame copy are this file's
half of address-space integrity under sharing. The decisions live here; the
copy-on-write machinery itself is [[sub-kernel-burrow]]'s and the address-space
object's.

**[[inv-i22]]** and **[[inv-i27]]** — several handlers carry identity and
console-trust gates. They are enforced here in the sense that this is where the
check is written; the authority model itself is [[moc-kernel-security]]'s.

## Error paths

Every handler returns a negative on failure and the dispatcher writes it back
unexamined — there is no central error translation. The unknown-number arm
returns `-1`; the source notes that a signal-equivalent notification is the
eventual behaviour and was not built.

The device-error clamp is the one piece of central error handling, and it lives
in the shared read and write bodies rather than in the dispatcher.

## Performance

The staging tiers are the measured surface: raising the transfer maximum from
4 KiB to 128 KiB was a throughput fix, not a cleanup, and the stack tier is
preserved precisely so the small-operation path pays nothing for it.

The zero-copy network fast path bypasses staging entirely when a large
transfer's buffer lies inside a flow's shared ring, and is gated on a size
threshold so small transfers never pay the extra handle lookup.

## Prosecution

- **A new syscall's authority gate goes in the inner, not the handler**, unless
  the syscall has no inner. An inner is separately callable; a gate above it is
  advisory to anyone who does not come through the handler.
- **A new handler reads only `x0..x5`** — unless the frame is the *subject* of
  the call, which is a short and closed list. Taking `ctx` to read a seventh
  argument is the wrong reason; taking it to rewrite, copy, or restore the frame
  is the only right one, and it obliges the dispatch arm to say what it does with
  the result.
- **Every arm returns, and a never-returning handler is marked.** The switch has
  no fallthrough and gains none; the three arms without a `return` are protected
  by `noreturn` at the handler plus an `extinction()` backstop, not by the
  reader's attention.
- **A gate belongs below every front end that can reach it.** Where a syscall
  has two argument shapes, the gate goes in the shared core even when that costs
  a wasted copy on a refusing path — a gate in one front end is a gate the other
  does not pass, which is the handler/inner rule restated for the core split.
- **A new phenotype row confers shape, not authority.** A translation may
  renumber and remap arguments; it may not read or write the capability word, and
  the native gate it lands on must be the same one a native caller meets.
- **A new hardware-mint subtype does not become a fourth copy.** The
  CreateBegin/CreateCommit pair is the [[inv-i34]] enforcement; three identical
  copies of it already exist and nothing pins them together.
- **A new staging site charges before allocating and uncharges on every exit**,
  including the fault and error paths, and must not reassign the length variable
  between the two while an allocation is held.
- **A new user-pointer use is validated with the length actually touched**, not
  with the caller's buffer size — see Caveats for the sibling pair that
  disagrees about this.
- **A lookup's reference is released on every path.** The helpers transfer
  ownership; a missed release is a leak that outlives the call.
- **A capability is checked at mint.** Prefer gating later operations on the
  kernel-minted object type over re-checking the capability, which must be
  remembered at each new site.

## Seams

- **The two error conventions are not converging, and the newest arrivals took
  opposite sides.** New syscalls use errno, old ones the bare sentinel, and a
  caller cannot tell which from the number alone. The three syscalls added since
  this dossier was written split down that seam by *lineage* rather than by date:
  execve and rfork return errno throughout, while the GPU-buffer mint returns the
  bare sentinel — because it was written as a copy of the weave mint, which was
  written as a copy of the plain DMA mint. The convention propagates along the
  copy, not along the calendar.
- **The unknown-syscall arm has no notification.** Documented as owed. It is
  also the one hole in the phenotype layer's otherwise careful four-way naming of
  refusals: a *translated* number that had no native arm would land here and
  return the sentinel with no unserved report at all. Nothing constructs that
  case today — every native symbol the table can emit has an arm, and a
  compile-time assertion pins the native/Linux boundary constant — but the
  guarantee is a census, not a mechanism.
- **execve cannot say "not executable".** POSIX's `ENOEXEC` has no entry in the
  errno registry, so a load failure reports `EINVAL`. The registry is ABI-bearing
  and additions need signoff, so the gap is tracked rather than closed by fiat.
  It matters at the shell: `ENOEXEC` is how a shell learns to re-run a file as a
  script, and until the code exists a shell cannot make that distinction.
- **The environment's bounds answer a different error code than argv's, on
  purpose.** Over-long environment data answers `E2BIG` — a caller acts on the
  difference, since it says the request was well-formed and splitting will work —
  while the argv bounds beside it still answer `EINVAL`, because that is a landed
  ABI whose error code is reserved to the errno rollout rather than changed as a
  side effect. The asymmetry is deliberate and recorded.
- **A debugger cannot publish a target's instruction cache.** The publish call
  resolves the *caller's* process, so a cross-process write into a JIT region —
  which the debug surface permits — cannot be followed by the maintenance that
  makes it visible. The source names this as a missing kernel mechanism rather
  than a caller error, which is the correct reading: it is the composition of
  two surfaces that were each sound alone.

## Caveats

- **Two syscalls of identical shape disagree about oversized buffers, and the
  disagreement is a fix applied to one of them.** Both the working-directory
  read and the descriptor-name read copy a bounded kernel string into a user
  buffer and return its length. The working-directory one computes the string
  first, then validates and copies *exactly* the bytes it will write, and
  carries a comment explaining why: the earlier version rejected any buffer
  larger than the path maximum, which broke every caller using the
  near-universal idiom of passing a page-sized buffer — the comment names the
  build tools it broke and the error they emitted.

  The descriptor-name read still has both halves of that rule. It rejects a
  buffer larger than the path maximum, and it validates the caller's *whole*
  buffer rather than the handful of bytes it writes. Neither is load-bearing:
  the real fit check happens twenty lines later against the actual string
  length, and the validator is overflow-safe without an upper bound. All three
  copies of the ABI document only the too-*small* failure; none mentions
  too-large.

  It is unreached today — the sole caller is a boot probe with a 64-byte
  buffer — and the caller class the fix's own comment enumerates is exactly the
  one that would meet it. The rule was written down as two coupled corrections
  with their rationale, and landed on one of the two places that had it.

- **One authority gate sits in the handler above a separately-callable inner.**
  The console-open gate — the check that only the console-trust anchor may take
  the single-reader console — is in the handler, while its inner is non-static
  and reachable. No production caller uses the inner today; the kernel test
  suite does, which is legitimate and is also the demonstration that the bypass
  exists. A future kernel-side console consumer would acquire the descriptor
  without the check.

- **The validator's zero-length pass is a range property, not a pointer
  property.** A null pointer with zero length validates successfully. Every
  current caller pairs it with the length it will touch, so this is correct; a
  caller that validated with zero and then dereferenced would be unprotected,
  and nothing prevents that shape.

- **The [[inv-i34]] mint sequence exists in three copies here, and the same
  three-member family is factored one layer down.** The plain, weave, and
  GPU-buffer create handlers are twenty-six lines each and differ in **exactly
  one token** — which mint function they call. Everything else is identical: the
  capability load, the rights validation, the zero-size rejection, the
  CreateBegin permit, the CreateCommit install-under-recheck, and the
  release-on-lost-race.

  Two things make this worth recording rather than filing as taste. First, the
  object layer beneath already solved it: its three creates are one-line wrappers
  over a shared body parameterised by size envelope and subtype. So the same
  family is parameterised on one side of this boundary and copied on the other,
  and the copied side is the one holding the invariant. An I-34 correction — a
  reordering of the two steps, an added axis, a fix to the revoke re-check —
  must be applied three times, and the third omission is silent.

  Second, **the comments describe the factored version, not this one.** Both
  derived handlers say they differ from their model "only in the size envelope
  and the minted subtype bit" — but the envelope is not in the handler at all.
  It is an argument to the shared body one layer down. The sentence is an
  accurate description of `dma_handle.c` sitting on top of `syscall.c`'s copy,
  which is how a reader is most likely to believe the parameterisation is here.

## Provenance

[[chg-2026-08-03-syscall-dispatch-sweep]] established the file.
[[chg-2026-08-15-syscall-dispatch-lineage]] is the re-sweep after ~3500 lines
moved: the phenotype prologue, the three frame-taking handlers, the core/front-end
split, and the payer attribution.

## A diagnostic on this path emits ONE unit, never a run of `uart_*` calls (2026-08-18)

`uart_puts` is a bare per-character loop that takes **no lock**, so a direct
emitter takes neither the console writer role nor the TX ring lock, and reaches
the UART by a road neither serializer gates. Two consequences, both live:

- It interleaves at BYTE granularity with any concurrent console writer. This
  was observed, not theorised: #76 removed the same loop from `SYS_PUTS` after
  a login prompt came out as `patapestrssyd: mworodd:e`.
- It bypasses the extinction ring claim (`cons_tx_claim_for_dump`), whose hold
  stops every ring producer and the drain but cannot stop a peer writing the
  FIFO directly -- so those bytes can land inside an `EXTINCTION:` line, which
  costs the multi-boot classifier a corruption verdict and can invert a
  `test-fault.sh` result.

`viv_report_unserved` was the second instance (main#243), and the worse one:
an unprivileged EL0 program chooses when it fires by issuing a syscall the
phenotype does not serve. It now builds one `cons_diag_line` and emits it in a
single push. The volume is bounded independently -- deduped per syscall number,
and `g_viv_unserved_reports` caps the total at 96 for the whole boot and is NOT
reset when the census owner changes, so spawning cannot re-arm it.

**The rule for any new diagnostic on the dispatch path**: build a
`cons_diag_line` and emit once. The remaining direct `uart_*` emitters in
`syscall.c`, `sched.c`, `exec.c`, `9p_client.c` and friends are the residual
(main#243) and are NOT a mechanical sweep -- boot-time and crash-path emitters
are deliberately raw, because the ring is unarmed or its lock may be held by a
dying peer.
