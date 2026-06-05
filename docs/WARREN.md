# Warren — a shared-memory ring transport for 9P (post-v1.0 idea)

**Status**: IDEA CAPTURE, not a committed design. 2026-06-05. Origin: a design
conversation about io_uring and how it maps onto Thylacine's 9P philosophy.
**Sequence**: post-v1.0 (v1.x+). Hard prerequisite: the *synchronous* 9P path
must be rock-solid first — it now is (the #841 elected-reader pipelining + the
deep-smp-review SMP soundness arc). This document exists so we can pick the idea
up cleanly when we're ready; nothing here is binding, and building it needs the
usual design-conversation → scripture-commit → spec → impl → audit cadence.

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

> **Warren is the inversion of io_uring.** Rather than import io_uring's
> opcode zoo for Linux compat, expose our existing 9P client's pipelining to
> userspace via a shared-memory submission/completion ring. Userspace posts
> 9P-shaped ops — `(fid, opcode, a buffer slice in a registered Burrow, tag)` —
> into an SQ ring; the kernel's 9P client drives them; R-messages return as CQEs
> into the CQ ring. The "opcodes" are just the 9P operation set
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

---

## 2. The structural parallel to 9P

| io_uring | 9P (what Thylacine already runs) |
|---|---|
| SQE (op descriptor) | T-message (Twalk / Tlopen / Tread / Twrite / Tcreate / …) |
| CQE (completion) | R-message |
| `user_data` correlation token | **tag** |
| registered / fixed file | **fid** |
| out-of-order completion | out-of-order R-messages (tag-demux) |
| linked SQE chain | multi-element Twalk / a sequence of T-messages |
| SQPOLL shared-ring submission | *(the missing piece — we still trap per op)* |

The kernel 9P client (the #841 elected-reader work) already implements
multi-in-flight, tag-demux, lock-never-held-across-recv, and out-of-order
completion. The async pipelined *engine* exists; Warren supplies the *transport*
that lets userspace feed it without a trap per message.

---

## 3. The synthesis

Two ways to bring io_uring to Thylacine:

- **(A) Literal io_uring ABI for Linux compat.** A compat chore, not a
  philosophical fit, and it imports io_uring's single worst trait: a large,
  security-sensitive surface (io_uring has been one of Linux's richest CVE veins
  precisely because shared-memory + async + every-opcode is a lot of attack
  surface). For a capability-scoped, per-Proc-namespace OS with a "complexity
  only where verified" conviction, this is the wrong direction. If we ever want
  liburing-using Linux binaries to run, this can be a thin shim *over* Warren —
  but it is not the design center.

- **(B) Warren — a native ring transport for 9P.** The synthesis. Userspace
  posts 9P-shaped ops into an SQ ring; the existing client drives them;
  R-messages come back as CQEs. io_uring's batching + out-of-order completion +
  (with a poll thread) zero-syscall submission come essentially *for free*,
  because 9P was already a pipelined message protocol — we are only changing the
  transport from "trap-per-message" to "shared-ring".

The win over Linux's design: **no new opcode namespace.** 9P is the vocabulary,
and it is uniform across every resource the namespace can name.

---

## 4. Mapping onto existing Thylacine primitives

Warren is mostly *assembly of mechanisms we already have*, which is the strongest
signal that it fits:

- **The rings** → a shared **Burrow** (VMO) mapped into the Proc (the io_uring
  mmap'd-ring analog). `SYS_BURROW_ATTACH` / `SYS_BURROW_DETACH` exist; the
  Burrow refcount is now SMP-safe (#847). The SQ and CQ live in this Burrow.
- **Submission** → either a `SYS_WARREN_ENTER`-style batch trap (submit N, reap
  M), or an SQPOLL-style kernel kthread per ring polling the SQ tail for the
  zero-syscall hot path. The kthread is `cpu_pinned`-able and composes with the
  redesigned scheduler.
- **The engine** → the #841 client's tag/fid pipelining, unchanged. Each SQE
  maps to a 9P T-message on the session the `fid` belongs to; the client assigns
  a wire tag, sends it, and demuxes the reply.
- **Completion** → R-messages become CQEs posted to the CQ ring with the
  userspace tag; the wakeup rides the existing poll / Rendez / devnotes
  machinery. Death-interruptible sleep (#811) composes — a Proc tearing down with
  ops in flight unwinds cleanly.
- **Registered buffers / fixed files** → a pinned Burrow region for buffers; a
  set of pre-walked fids cached in the ring's context (the "fixed files"
  analog). The capability scoping is *already* there: a fid is a capability-
  scoped handle, and the 9P session is bound at attach.
- **Linked ops** → this is where it gets *very* Plan 9. A chain
  `Twalk → Tlopen → Tread → Tclunk` submitted as one unit is literally a tiny 9P
  program. Plan 9 already does multi-element walks in one message; Warren
  generalizes that to arbitrary op chains — collapsing a whole file access to
  ~zero traps on the hot path.

Against the capability-microkernel SOTA: Fuchsia/Zircon (channels + ports +
FIDL), seL4 (endpoints + notifications), Genode (RPC + signals + dataspaces) all
have async-completion-over-shared-memory in pieces, none with io_uring's
batch-ring. Warren is the fusion: io_uring's **ring transport** + 9P's **message
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
devices, `/proc`, and `/srv` services, not just the block/socket cases.

---

## 6. Soundness obligations (where it fights our convictions — prosecute hard)

Shared-memory async is a soundness/security minefield; this is a spec-first,
audit-bearing surface from day one. The known hazards (and why our model helps):

1. **Ring TOCTOU + no-lost / no-double / no-stale completion.** A TLA+ model of
   the SQ/CQ state machine (`specs/warren.tla`) with those invariants, in the
   lineage of `poll.tla` + `9p_client.tla`. The submission/completion ring is a
   wait/wake state machine; model it like one.
2. **Check at submission, pin for the op's lifetime.** io_uring's reputational
   problem was decoupling the credential check from the work (which ran in a
   kernel worker against possibly-changed state). Warren must evaluate the
   I-2 / I-6 rights at *submit* time and **pin** them (snapshot) for the op's
   lifetime — never re-evaluate at completion (which races a `clunk`). The #844
   handle-lifetime pass (a by-value snapshot with the object refcount held) is
   the substrate; the fid + session being capability-scoped + attach-bound is
   what makes this cleaner than Linux's.
3. **The kernel writes into a user buffer at completion time.** The target
   Burrow region must stay mapped + owned for the op's lifetime; a Proc that
   detaches the Burrow or changes its namespace mid-flight must not cause a
   stale write. Burrow refcounting (#847) + a per-op hold is the mechanism.
4. **Per-fid ordering.** 9P allows out-of-order R-messages, but some sequences
   need ordering (write-then-read of the same fid). Warren needs io_uring's
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

## 7. Sequencing + relation to the committed angles

- **Post-v1.0 (v1.x+).** Warren is a performance/ergonomics transport layered on
  top of a synchronous 9P path that must be solid first. That path is now solid
  (the #841 pipelining + the SMP soundness arc), so the prerequisite is met — but
  the roadmap (textual OS → hardening → Halcyon) should not be derailed for it.
- **Builds on NOVEL #1** (9P totalized — Warren is only uniform *because* 9P is)
  and **NOVEL #3** (the pipelined 9P client with out-of-order completion — Warren
  is its userspace-facing transport). It is naturally a "designed-not-implemented
  v2.0 contract" in the spirit of NOVEL #9.
- It does **not** require a new IPC mechanism — Thylacine has exactly one
  composition mechanism (9P), and Warren is a faster on-ramp to it, not a rival.

---

## 8. Naming

**Warren**: a network of connected burrows sharing passages. Thylacine's
shared-memory ring transport is built on **Burrow** (VMO) dataspaces, and a
warren is the system of burrows through which messages flow between userspace and
the kernel's 9P engine — a network of shared-memory passages. It sits in both
lineages: the marsupial/biology theme (burrow → warren) and the Plan 9 heritage
(9P-as-messages). Held as the proposed name; load-bearing identifiers get
signoff before they land, but the user has approved "Warren" for this idea.

---

## 9. Open questions / a "done" sketch (for when we pick it up)

- SQPOLL vs batch-enter as the v1.x-first mode (probably batch-enter first; the
  poll-thread is the optimization).
- Whether Warren ops are literal wire T-messages or a Thylacine-native op
  descriptor that the kernel translates to T-messages (the latter keeps the
  userspace ABI stable across 9P dialect changes).
- The registered-fid model vs walking per-op (registered fids are the win, but
  add lifetime bookkeeping).
- Multishot ops (one SQE → many CQEs, e.g. a `/srv` accept loop) — a natural fit
  for a service that accepts many connections, but more invariant surface.
- Linux-compat liburing shim *over* Warren — explicitly out of the v1.x core; a
  later, optional layer.

**"Done" (v1.x):** a Burrow-backed SQ/CQ ring; `SYS_WARREN_*` setup + enter; the
9P op set as SQEs; out-of-order CQE completion via the #841 client; submit-time
capability pin; `specs/warren.tla` green with the no-lost/no-double/no-stale
invariants + a buggy-cfg counterexample; a latency benchmark showing the
trap-amortization win on a high-fanout workload (globbing / many-connection
server); the audit-trigger surfaces extended to cover it.

---

## Cross-references

- `docs/NOVEL.md` §3.1 (Angle #1, 9P totalized) + §3.3 (Angle #3, pipelined 9P
  client) — the foundations Warren rests on.
- `docs/reference/*` 9P client (the #841 elected-reader engine) + Burrow/VMO +
  poll + notes — the primitives Warren assembles.
- `docs/ARCHITECTURE.md` §21 (9P client) + the BURROW/VMO + poll/futex sections.
- io_uring background: `Documentation/io_uring.7`, the liburing project, and
  Axboe's "Efficient IO with io_uring" paper.
