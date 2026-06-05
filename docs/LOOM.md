# Loom — a shared-memory ring transport for 9P (the io_uring inversion)

**Status: binding design — SIGNED OFF 2026-06-05.** Pre-Utopia arc **2 of 2**
(after Lazarus M1; `docs/PORTABILITY.md` + ROADMAP §8.0a). Origin: a design
conversation about io_uring and how it maps onto Thylacine's 9P philosophy
(captured as `WARREN.md`, renamed Loom 2026-06-05). The hard prerequisite — a
rock-solid *synchronous* pipelined 9P path — is met (the #841 elected-reader
work + the deep-smp-review SMP soundness arc). Spec-first is **re-enabled** for
this surface (`specs/loom.tla` gates impl). Builds as an arc of spec'd + audited
sub-chunks (§10); no impl lands until `specs/loom.tla` is TLC-green.

Why pre-Utopia (pulled forward from its documented post-v1.0 slot): Utopia is
where userspace apps arrive, and Loom's value lands first on the **native**
side (libthyla-rs — `ut`, the coreutils, the servers we write). Landing Loom
before the apps that consume it means they get fast IO from the start.

---

## Thesis

io_uring is Linux's async I/O interface: two shared-memory ring buffers (a
submission queue and a completion queue) that turn the kernel/userspace syscall
boundary into a message queue, so I/O is *described as data* and submitted in
batches (or, with a kernel poll thread, with **zero syscalls** on the hot path).

Thylacine already has the *semantic* half of that — 9P is a pipelined,
tag/fid-addressed, out-of-order-completion message protocol, and our kernel 9P
client already drives it that way (multi-in-flight, tag-demux, no lock across
recv). What we don't have is io_uring's *transport*: the shared-memory ring that
amortizes (or eliminates) the trap.

> **Loom is the inversion of io_uring.** Rather than import io_uring's opcode
> zoo for Linux compat, expose our existing 9P client's pipelining to userspace
> via a shared-memory submission/completion ring. Userspace posts 9P-shaped ops
> — `(opcode, fid, a buffer slice in a registered Burrow, user_data)` — into an
> SQ ring; the kernel's 9P client drives them; R-messages return as CQEs into the
> CQ ring. The "opcodes" are just the 9P operation set
> (walk/open/read/write/create/clunk/stat/…) — small, already audited, already
> the universal I/O vocabulary.

Linux's hardest io_uring design question was *"what is the operation
vocabulary?"*, which forced a parallel opcode namespace bolted onto a
non-uniform syscall surface. For Thylacine that question **dissolves**: because
everything is a file and file ops are 9P messages, 9P *is* the uniform
vocabulary, and an async batching layer for it covers files, `/net`, `/proc`,
`/srv` services, and devices uniformly — not just block + sockets, which is
effectively all Linux's io_uring privileges. Plan 9's "I/O is messages" makes
io_uring's worst problem disappear.

---

## 1. What io_uring is (and why it exists)

io_uring (Jens Axboe, Linux 5.1, 2019) exists because every prior Linux async
story was poor: synchronous syscalls are one-trap-per-op; `epoll` reports only
*readiness* (and treats regular files as always-ready); POSIX AIO was a
thread-pool emulation. The mechanism:

- **Two SPSC ring buffers** mmap'd between userspace and kernel: the Submission
  Queue (SQEs) and Completion Queue (CQEs).
- Userspace writes op descriptors (opcode + fd + buffer + offset + a `user_data`
  correlation token) into the SQ and bumps the tail; the kernel consumes them,
  runs the ops (often async), and posts CQEs (result + the same `user_data`).
- **One `io_uring_enter()` submits N ops and reaps M completions.** With
  **SQPOLL**, a kernel thread polls the SQ, so steady-state submission is **zero
  syscalls** — memory stores + a barrier.
- Completions return **out of order**, matched by `user_data`. SQEs can be
  **linked** (B runs after A) — a small dependency graph submitted as one unit.
  Buffers/files can be **pre-registered** to skip per-op pinning/refcount.

The deep idea: **the syscall boundary becomes a message queue**, decoupling
*submission* from *completion* and letting the kernel batch/reorder/pipeline.

io_uring's reputational cost: it has been one of Linux's richest CVE veins —
shared-memory + async + every-opcode is a large, security-sensitive surface, and
it is *disabled by default* in many hardened environments. That cost is the
warning Loom's spec-first + audit cadence answers (§6), not a reason to avoid the
mechanism.

---

## 2. The structural parallel to 9P

| io_uring | 9P (what Thylacine already runs) |
|---|---|
| SQE (op descriptor) | T-message (Twalk / Tlopen / Tread / Twrite / Tcreate / …) |
| CQE (completion) | R-message |
| `user_data` correlation token | **tag** |
| registered / fixed file | **fid** (a capability-scoped, attach-bound handle) |
| out-of-order completion | out-of-order R-messages (tag-demux) |
| linked SQE chain | multi-element Twalk / a sequence of T-messages |
| SQPOLL shared-ring submission | *(the missing piece — we still trap per op)* |

The kernel 9P client (the #841 elected-reader work) already implements
multi-in-flight, tag-demux, lock-never-held-across-recv, and out-of-order
completion. The async pipelined *engine* exists; Loom supplies the *transport*
that lets userspace feed it without a trap per message.

---

## 3. The synthesis

Two ways to bring io_uring to Thylacine:

- **(A) Literal io_uring ABI for Linux compat.** A compat chore, not a
  philosophical fit, and it imports io_uring's single worst trait: a large,
  security-sensitive surface. For a capability-scoped, per-Proc-namespace OS
  with a "complexity only where verified" conviction, this is the wrong
  direction. If we ever want liburing-using Linux binaries to run, this can be a
  thin shim *over* Loom — but it is **not** the design center, and the Pouch
  port targets that matter for v1.0 use *zero* io_uring (the relevant programs —
  stratumd, libsodium, Helix, later git/ssh/python — all run on pouch's
  synchronous, 9P-backed POSIX surface unchanged). The shim stays out of core
  (§9, §10).

- **(B) Loom — a native ring transport for 9P.** The synthesis. Userspace posts
  9P-shaped ops into an SQ ring; the existing client drives them; R-messages
  come back as CQEs. io_uring's batching + out-of-order completion + (with a poll
  thread) zero-syscall submission come essentially *for free*, because 9P was
  already a pipelined message protocol — we are only changing the transport from
  "trap-per-message" to "shared-ring".

The win over Linux's design: **no new opcode namespace.** 9P is the vocabulary,
and it is uniform across every resource the namespace can name. **(B) is the
chosen path.**

---

## 4. Mapping onto existing Thylacine primitives

Loom is mostly *assembly of mechanisms we already have*, which is the strongest
signal that it fits:

- **The rings** → a shared **Burrow** (VMO) mapped into the Proc (the io_uring
  mmap'd-ring analog). `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` exist; the
  Burrow refcount is now SMP-safe (#847). The SQ index ring, the SQE array, and
  the CQ ring live in this Burrow; the kernel reaches the same physical pages via
  the direct map while userspace sees its mapping.
- **Submission** → either a `SYS_LOOM_ENTER`-style batch trap (submit N, reap M),
  or an SQPOLL-style kernel kthread per ring polling the SQ tail for the
  zero-syscall hot path. The kthread is `cpu_pinned`-able and composes with the
  redesigned scheduler.
- **The engine** → the #841 client's tag/fid pipelining (`inflight[tag]`, the
  elected reader, the demux). The completion action becomes **pluggable**
  (§8.4): the existing synchronous path wakes a stack `rendez`; the Loom path
  posts a CQE. One engine, two front-ends.
- **Completion** → R-messages become CQEs posted to the CQ ring with the
  userspace `user_data`; the wakeup rides the existing poll / Rendez / devnotes
  machinery. Death-interruptible sleep (#811) composes — a Proc tearing down with
  ops in flight unwinds cleanly.
- **Registered buffers / fixed files** → a pinned Burrow region for buffers; a
  set of pre-walked **registered handles** cached in the ring's context (the
  "fixed files" analog). The capability scoping is *already* there: a registered
  handle is a capability-scoped `KObj_Spoor`, and the 9P session is bound at
  attach. The #844 by-value handle snapshot is the submit-time pin substrate.
- **Linked ops** → this is where it gets *very* Plan 9. A chain
  `Twalk → Tlopen → Tread → Tclunk` submitted as one unit is literally a tiny 9P
  program. Plan 9 already does multi-element walks in one message; Loom
  generalizes that to arbitrary op chains — collapsing a whole file access to
  ~zero traps on the hot path.

Against the capability-microkernel SOTA: Fuchsia/Zircon (channels + ports +
FIDL), seL4 (endpoints + notifications), Genode (RPC + signals + dataspaces) all
have async-completion-over-shared-memory in pieces, none with io_uring's
batch-ring. Loom is the fusion: io_uring's **ring transport** + 9P's **message
semantics** + the capability/namespace model giving it the scoping the Linux
version lacks.

---

## 5. The latency payoff

VISION.md commits to a latency budget. The current path is trap-per-9P-op. A ring
transport amortizes the trap; SQPOLL eliminates it on the hot path. This matters
most exactly where it hurts today:

- a shell globbing a directory = many walks/stats,
- a server fanning out across many connections,
- a bulk copy = many read/write pairs,

and — because it is 9P underneath — it applies uniformly to files, `/net`,
devices, `/proc`, and `/srv` services, not just the block/socket cases. The SQE
*format* is second-order here (native vs wire both win via trap amortization;
§7); the win is the ring transport itself.

---

## 6. Soundness obligations (where it fights our convictions — prosecute hard)

Shared-memory async is a soundness/security minefield; this is a spec-first,
audit-bearing surface from day one. The known hazards (and why our model helps):

1. **Ring TOCTOU + no-lost / no-double / no-stale completion.** A TLA+ model of
   the SQ/CQ state machine (`specs/loom.tla`) with those invariants, in the
   lineage of `poll.tla` + `9p_client.tla`. The submission/completion ring is a
   wait/wake state machine; model it like one. The kernel **copies SQE fields to
   kernel memory before validating/acting** — never re-reads a shared-ring field
   after the check (userspace can mutate it concurrently).
2. **Check at submission, pin for the op's lifetime.** io_uring's reputational
   problem was decoupling the credential check from the work (which ran in a
   kernel worker against possibly-changed state). Loom evaluates the I-2 / I-6
   rights at *submit* time and **pins** them (the #844 by-value snapshot, object
   refcount held) for the op's lifetime — **never** re-evaluates at completion
   (which races a `clunk`). The fid + session being capability-scoped +
   attach-bound is what makes this cleaner than Linux's.
3. **The kernel writes into a user buffer at completion time.** The target Burrow
   region must stay mapped + owned for the op's lifetime; a Proc that detaches
   the Burrow or changes its namespace mid-flight must not cause a stale write.
   Burrow refcounting (#847) + a per-op hold is the mechanism.
4. **Per-fid ordering.** 9P allows out-of-order R-messages, but some sequences
   need ordering (write-then-read of the same fid). Loom needs io_uring's
   `LINK` / `DRAIN` analogs, and must respect the client's per-fid serialization
   where the protocol requires it.
5. **Resource exhaustion.** Bounded ring sizes; bounded in-flight tags per
   session (the tag pool is already finite, I-10); back-pressure when the CQ is
   full (the F3/F5 9P-client "send is all-or-nothing-fail" discipline is the
   model).

The lesson from io_uring's CVE history is not "don't build it" — it is "the
shared-memory async boundary is exactly the kind of load-bearing invariant the
spec-first + adversarial-audit cadence exists for."

---

## 7. Resolved design decisions (user votes, 2026-06-05)

Grounded against the tree (`kernel/include/thylacine/9p_client.h` is the
structured client API the design rests on):

- **SQE format = native op-descriptor core + a *designed* wire-passthrough
  seam.** The client API is already structured —
  `p9_client_read(fid, offset, count, buf)`, walk, lopen, lcreate, getattr, … —
  so an SQE is a native descriptor dispatched straight to `p9_client_<op>`.
  Native is faster-or-equal on the hot path (fixed-size SQE → better ring cache
  behavior; the kernel *encodes* in a trusted buffer instead of *parsing*
  untrusted wire), smaller attack surface, and ABI-stable across 9P dialect
  changes. Literal-wire's only genuine edge is a pure 9P *proxy* that already
  holds wire bytes — captured by a **reserved** `LOOM_OP_WIRE_PASSTHROUGH`
  opcode that is *designed in the ABI + spec but not built* until a real proxy
  consumer exists (building an untrusted-wire parser with no consumer is exactly
  the unverified complexity the convergence bar forbids).
- **Scope = the maximal build.** SQPOLL (the zero-syscall poll-thread) and
  multishot (one SQE → many CQEs, e.g. a `/srv` accept loop) are built **up
  front**, each as its own spec'd + audited sub-chunk (§10), not deferred. The
  invariant surface each adds is *modeled in `specs/loom.tla`* and audited, not
  hand-waved.
- **Spec-first = re-enabled for this surface.** `specs/loom.tla` (clean +
  buggy-cfg counterexamples) is the gate before each impl sub-chunk — the
  SMP-scheduler model-first pattern (2026-06-05 precedent). The shared-memory
  async boundary is precisely what spec-first exists for. This is a case-(a)
  re-enabling of the broadly-suspended spec-to-code policy (CLAUDE.md).
- **Name = Loom** (§12). **liburing-compat shim = out of the v1.x core** (§9).

---

## 8. ABI sketch (design-level; exact layout + `_Static_assert`s land with the spec + impl)

### 8.1 Setup + registration

- `SYS_LOOM_SETUP(u32 entries, struct loom_params *params) -> loom_fd`
  — allocates the ring **Burrow** holding: the SQ index ring (`u32[entries]`,
  the io_uring-style submission-order indirection), the SQE array
  (`entries * sizeof(loom_sqe)`), the CQ ring, and the head/tail control words
  for each. `params` reports the Burrow handle + the byte offsets/sizes of each
  region so userspace maps it (the ring memory is the shared Burrow; both sides
  see the same pages). Returns a new `KObj_Loom` handle. `flags`:
  `LOOM_SETUP_SQPOLL` (start a `cpu_pinned`-able kernel poll-thread),
  `LOOM_SETUP_CQSIZE`, …
- `SYS_LOOM_REGISTER(loom_fd, u32 op, const void *arg, u32 nargs) -> r`
  — `LOOM_REGISTER_HANDLES`: install an array of `KObj_Spoor` handles into the
  ring's fixed-handle table (the registered-fid / "fixed files" analog); each is
  resolved once to `(p9_client *, fid)` + a rights snapshot.
  `LOOM_REGISTER_BUFFERS`: pin Burrow regions for zero-copy payload.

### 8.2 Submit + reap

- `SYS_LOOM_ENTER(loom_fd, u32 to_submit, u32 min_complete, u32 flags) -> n`
  — consume up to `to_submit` SQEs from the SQ (in SQ-index order), dispatch
  each, then block until at least `min_complete` CQEs are available (or return
  per `LOOM_ENTER_NONBLOCK`; the wait is death-interruptible, #811). With SQPOLL,
  submission is automatic; `ENTER` is used only to wake an idled poll-thread or
  to wait for completions (the zero-syscall steady state).

### 8.3 Ring entries (design-level)

- `loom_sqe` (fixed size, `_Static_assert`'d at impl): `{ u8 opcode; u8 flags
  (LINK / DRAIN / CQE-skip / multishot); u16 resv; u32 handle_idx (registered-
  handle index, or LOOM_HANDLE_RAW); u64 offset; u32 len; u32 buf_idx_or_off;
  u64 user_data; … opcode-specific fields (e.g. walk names reference a
  registered-buffer slice) }`.
- `loom_cqe`: `{ u64 user_data; s32 result; u32 flags (LOOM_CQE_MORE for
  multishot) }`. `result >= 0` = byte count / packed qid / 0; `result < 0` =
  `-errno` (the `Rlerror` passthrough, mapped by the client's existing errno
  convention).
- `opcode` set = the `p9_client_*` surface: `LOOM_OP_{WALK, LOPEN, LCREATE,
  READ, WRITE, GETATTR, SETATTR, READDIR, FSYNC, CLUNK, RENAMEAT, UNLINKAT,
  MKDIR, SYMLINK, LINK, MKNOD, READLINK, STATFS}` + `LOOM_OP_WIRE_PASSTHROUGH`
  (**reserved, not implemented at v1.0** — the designed seam, §7).

### 8.4 The pluggable-completion refactor (the core kernel work)

Today every `p9_client_*` call blocks its submitter on a stack `rendez` until the
matching reply; the pipelining (`inflight[tag]`, the elected reader, the demux —
lock dropped across recv, #841) is internal and **reusable**. Loom adds a
**completion-kind** to the in-flight op:

- `WAKE_RENDEZ` — the existing synchronous `p9_client_*` path (unchanged).
- `POST_CQE` — when the reply is demuxed, write a `loom_cqe` (the op's
  `user_data` + the mapped `result`) into the CQ ring and signal the ring's wait.

The elected-reader / demux machinery is **unchanged**; only the completion action
becomes pluggable. This keeps the #841 client one engine with two front-ends. The
refactor touches the audited #841 surface, so it is audit-bearing (§9).

### 8.5 Submit-time pin (the I-30 mechanism)

At SQE-consume time (in `SYS_LOOM_ENTER` or the SQPOLL kthread), resolve
`handle_idx` → the registered handle's `(client, fid)` + snapshot its rights via
the #844 by-value handle snapshot (object refcount held for the op's lifetime).
The op carries the snapshot; completion **never** re-resolves the handle, so a
concurrent `clunk`/`close` cannot race it. The buffer slice is validated against
the registered-buffer table at submit and the Burrow held (#847) for the op's
lifetime.

---

## 9. Invariants + audit-trigger surface

Loom reserves two ARCH §28 invariants (the table edit lands *with* impl, the way
Lazarus defers its §28 edit to W1):

- **I-29 — Loom completion integrity.** Every *submitted* SQE produces *exactly
  one* terminal CQE (no lost, no double); no CQE is posted whose `user_data`
  correlation is stale (an abandoned/torn-down op never surfaces as a live
  completion). Modeled in `specs/loom.tla`.
- **I-30 — Loom submit-time capability pin.** The rights governing an op are
  evaluated + snapshotted at *submission* and held for the op's lifetime; never
  re-evaluated at completion. Enforced by the #844 snapshot + the
  registered-handle resolution at submit (§8.5).

Plus the obligations of §6 (ring TOCTOU; per-fid ordering via LINK/DRAIN; bounded
rings + the finite tag pool I-10 + CQ back-pressure).

**Audit-trigger surface** (row added to ARCH §25.4 + CLAUDE.md when impl lands):
the pluggable-completion refactor of the #841 client; the new shared-memory async
boundary (SQ/CQ ring); the Burrow-backed ring lifecycle; the submit-time pin; the
SQPOLL kthread; multishot. AEGIS/mallocng-adjacent only insofar as it drives the
same write path — but the shared-memory async boundary is its own first-class
hazard class.

---

## 10. Sub-chunk decomposition (each spec-gated + audited)

- **Loom-0 — scripture** (this commit): `LOOM.md` + the NOVEL.md promotion +
  the ROADMAP §8.0a/§12.2 registration. No code.
- **Loom-1 — model**: `specs/loom.tla` — the SQ/CQ state machine + I-29
  (no-lost/no-double/no-stale) + I-30 (submit-time pin) + ring TOCTOU; clean cfg
  + buggy-cfg counterexamples. **TLC-green gates every subsequent sub-chunk.**
- **Loom-2 — engine + ring**: the pluggable-completion refactor of the #841
  client (§8.4; audit-bearing) + `SYS_LOOM_SETUP` + the Burrow-backed SQ/CQ
  memory layout + the registered-handle table. Audit.
- **Loom-3 — batch-enter core**: `SYS_LOOM_ENTER` (submit N / reap M) + SQE →
  `p9_client_<op>` dispatch + the submit-time pin (§8.5) + CQE post +
  out-of-order completion. The core. Audit.
- **Loom-4 — SQPOLL**: the kernel poll-thread (zero-syscall hot path;
  `cpu_pinned`-able) — wait/wake + lifetime surface. Audit.
- **Loom-5 — multishot + linked ops**: one SQE → many CQEs (the `/srv`
  accept-loop) + LINK/DRAIN per-fid ordering. Audit.
- **Loom-6 — registered buffers + native API + bench**: pinned Burrow regions
  (zero-copy payload) + the libthyla-rs Loom wrapper (the native userspace API) +
  a latency benchmark on a high-fanout workload (globbing / many-connection
  server) showing the trap-amortization win. The `LOOM_OP_WIRE_PASSTHROUGH` seam
  stays reserved (designed, not built). Final audit + arc close.

---

## 11. Sequencing + relation to the committed angles

- **Pre-Utopia (scheduled 2026-06-05).** Loom is the 2nd of two arcs before
  Phase 7 resumes (Lazarus M1 first; ROADMAP §8.0a). Pulled forward from its
  documented post-v1.0 slot (§12.2) because Utopia's native userspace apps
  consume it. The prerequisite — a solid synchronous pipelined 9P path — is met
  (#841 + the SMP soundness arc).
- **Builds on NOVEL #1** (9P totalized — Loom is only uniform *because* 9P is)
  and **NOVEL #3** (the pipelined 9P client with out-of-order completion — Loom
  is its userspace-facing transport).
- It does **not** require a new IPC mechanism — Thylacine has exactly one
  composition mechanism (9P), and Loom is a faster on-ramp to it, not a rival.

---

## 12. Naming

**Loom**: a loom interlaces many threads — warp and weft — through one frame into
a single fabric. Loom interlaces many concurrent 9P operations (each a *thread*
of I/O in flight) through one shared-memory ring and the single elected-reader
engine into one woven I/O stream: the ops run in parallel, the loom is the frame
that orders and completes them. It sits in the Plan 9 "I/O is messages" lineage
(the ring weaves messages) and is apt for a many-in-flight transport. The rings
themselves still ride **Burrow** dataspaces (the substrate the predecessor name
"Warren" — a network of burrows — pointed at); Loom names the *weave* rather than
the substrate, which is the load-bearing idea. **Signed off (user) 2026-06-05**
(renamed from "Warren").

---

## Cross-references

- `docs/NOVEL.md` §3.1 (Angle #1, 9P totalized) + §3.3 (Angle #3, pipelined 9P
  client) — the foundations Loom rests on; §"Post-v1.0 candidates" (the promoted
  Loom entry).
- `docs/ROADMAP.md` §8.0a (the pre-Utopia arc registration) + §12.2 (the
  superseded io_uring entry + the post-v1.0 liburing-shim).
- `docs/PORTABILITY.md` (Lazarus M1 — pre-Utopia arc 1 of 2).
- `kernel/include/thylacine/9p_client.h` (the structured client API Loom
  dispatches to) + `docs/reference/47-9p-client.md` (the #841 elected-reader
  engine) + the Burrow/VMO + poll + notes references — the primitives Loom
  assembles.
- `docs/ARCHITECTURE.md` §21 (9P client) + the BURROW/VMO + poll/futex sections.
- io_uring background: `Documentation/io_uring.7`, the liburing project, and
  Axboe's "Efficient IO with io_uring" paper.
