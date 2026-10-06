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
  invariant surface each adds is *modeled in the Loom spec suite* and audited, not
  hand-waved (the suite is `specs/loom.tla` + the Loom-5 additions
  `specs/loom_multishot.tla` + `specs/loom_order.tla`).
- **Spec-first = re-enabled for this surface.** The Loom spec suite (`loom.tla` +
  `loom_multishot.tla` + `loom_order.tla`; each clean + liveness + per-bug
  counterexample cfgs) is the gate before each impl sub-chunk — the SMP-scheduler
  model-first pattern (2026-06-05 precedent). A NEW mechanism extends the suite
  FIRST (a new focused module when the mechanism's state shape diverges from the
  core — e.g. multishot's multiset CQ vs the core's single-CQE-per-op `cq`), then
  the impl. The shared-memory async boundary is precisely what spec-first exists
  for. This is a case-(a) re-enabling of the broadly-suspended spec-to-code
  policy (CLAUDE.md).
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
  convention). `-EAGAIN` raised by the kernel itself means the op never left
  it: its session's tag pool or send ring was full. The session is intact and
  the SQE may be resubmitted (NET-DESIGN 12.2, "a shortage is not an answer").
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

### 8.5.1 Directory-mutation authority (the kernel DAC at submit; 2026-09-23)

The rights snapshot above is necessary for the child-mutation ops (`MKDIR`,
`MKNOD`, `SYMLINK`, `UNLINKAT`, `RENAMEAT`, `LINK`), but it is not sufficient.
Their directory handle is normally an `O_PATH` handle, which is born R|W with no
permission check on its target (it has to be: `O_PATH` is the create-from-a-base
pattern `SYS_WALK_CREATE` uses too). Its RIGHT_WRITE is therefore hollow. The
sync twins treat it that way: `SYS_WALK_CREATE`, `SYS_UNLINK` and `SYS_RENAME`
each run `perm_check(parent, W|X)` at the operation.

Loom-6b-2 shipped these ops on the rights gate alone, on the premise that "the
identity axis stays the dev9p server's". A-3b made that false. The kernel is
the only rwx enforcer (IDENTITY-DESIGN 3.7), and Stratum checks only dataset
scope. So any principal that could X-search to a directory could create,
unlink, rename, link or symlink entries in it. Found 2026-09-23 (aux); the
rule below closes it.

- **At submit, each mutated directory is checked like the sync twin.** Stat it
  (`spoor_stat_native`), then `perm_check(identity, st, W|X)`. `RENAMEAT`
  checks both directories; `LINK` checks the directory the link lands in. A
  stat failure answers `-EIO` and a denial `-EACCES`, inline, and nothing goes
  on the wire. The check is gated on `perm_enforced`, as the twins are.
- **The identity is the Loom's creator,** stamped at `SYS_LOOM_SETUP` before
  the handle publishes. `KObj_Loom` is non-transferable and non-dup-able (I-5;
  pinned in `handle.h`), so only the creator's threads can submit, and the
  creator's live identity (principal, groups, caps) is exactly the submitter's.
  It is read at submit, so a legate's `CAP_HOSTOWNER` counts only while the
  scope holds it (I-25).
- **An SQPOLL ring refuses these ops** (`-EOPNOTSUPP`). Its kthread submits,
  and the stat may be a wire RPC. A kthread blocked on a hung server (a
  partitioned Haul peer, say) would never reach the stop flag, `loom_free`'s
  join would never return, and its owner's exit would hang. The check cannot run
  there without blocking, so the ops fail closed there. This is the same
  disposition 8.5's SETATTR took for identity-setattr, and no SQPOLL consumer
  issues these ops. Mutation ops belong on a non-SQPOLL ring.
- **The create ops' group is the creator's own.** The SQE's gid (`MKDIR`/`SYMLINK`
  `_resv1[2]`, `MKNOD` `offset`) is resolved at submit:
  - 0 (`GID_INVALID`) means the creator's primary group, as the sync create
    always uses;
  - any other value must be a group the creator is in, unless it holds
    `CAP_HOSTOWNER` or `CAP_CHOWN`: perm_wstat_check's chgrp rule applied at
    birth;
  - on a caped session (IDENTITY-DESIGN.md 3.2, the identity cape) the group is
    the cape's: 0 sends the server `(u32)-1`, "leave it", and any other value is
    refused, since naming a group there is a chgrp and the cape refuses chgrp;
  - a refusal answers `-EACCES`, and a value above u32 `-EINVAL`.

  The resolved gid is written into the op's SQE snapshot, so the wire carries
  it and never the ring's bytes. (On a caped session `GETATTR` likewise hands
  userspace the cape's owner and group, with both `valid` bits set, so a ring
  and `SYS_FSTAT` report the same owner.)
- **Names are checked like the sync twins', on the copy the wire carries.**
  Each child name must be 1 to 255 bytes, hold no `/` or NUL, and not be `.`
  or `..` (`sys_copy_component`'s rule); a bad one answers `-EINVAL` before
  any stat. `SYMLINK`'s target is a path and is not a name. The names are
  copied out of the registered buffer into the op at submit, and the build
  sends that copy, so userspace cannot change a name after it was checked.
- **A create mode carries the rwx bits only.** `MKDIR` sends `mode & 0777`
  and `MKNOD` keeps its file type plus `mode & 0777`, as the sync create's
  `perm & 0777` does. Setuid, setgid and sticky are never honoured and
  `SYS_WSTAT` refuses them, so a create must not be a way to plant them.
- **`LCREATE` joins this gate when it is dispatched.** It creates a child too.
  Today it answers `-ENOSYS` with the other fid-lifecycle ops (the #916 seam),
  so it is not reachable.
- **The stat blocks, so admission is exact.** On a non-SQPOLL ring the stat runs
  in the submitter's own `ENTER`, like the sync twin's. There it may wait on a
  wire RPC between the op's consume and its disposition. The CQ admission gate
  used to count posted-unreaped plus in-flight ops only, so a sibling driver
  could not see an op in that gap and could over-admit into its slot. The
  post-time guard then drops a completion (I-29). That is the Loom-5 audit's F2
  residual, owed until now. Each consume or chain claim now holds its CQ slot in
  `admitting`, in the same lock hold as the room check, until its CQE posts, it
  goes in flight, or it parks HELD in the chain. The wait side counts it too:
  a blocking `ENTER` does not give up while a sibling is mid-submit (loom.tla's
  `CanStillComplete` counts that phase). With nothing in flight it sleeps
  instead of pumping, only while that is still the state, and every release
  of a reservation wakes the CQ waiters. A `DRAIN` waits for it as well: an
  op a sibling consumed and has not disposed of is a prior op, neither posted
  nor in flight, so the drain gate counts `admitting` beside in-flight and
  rearm-pending ops (loom_order.tla `DrainOrdered`).

### 8.6 SQPOLL — the poll-thread + the CQ wait-list (Loom-4)

**The design (Option 1, user-voted 2026-06-07).** With `LOOM_SETUP_SQPOLL`,
`SYS_LOOM_SETUP` starts a per-ring kernel poll-thread so steady-state submission
is **zero syscalls** — userspace writes SQEs + bumps `sq_tail`, the kthread drains
them. The two SQPOLL forks the design conversation resolved:

1. **The poll-thread's recv-wake + lifetime.** The kthread must drive the elected
   reader (recv → demux → CQE) so async completions appear without an `ENTER`
   syscall — but the reader's recv is a *blocking byte stream*, and #841 proved
   a deadline that fires **mid-frame** desynced it (the reader's count was its
   own until 2026-10-06; the client keeps it now, but a recv that times out
   still reads as a broken transport and kills the shared session). Option 1: the
   kthread is a `kproc()` thread (the `console_mgr` precedent), `cpu_pinned`-able,
   woken at idle/teardown by a **frame-boundary idle-deadline** — armed only when
   the recv is at a frame boundary (no bytes buffered for the current frame, where
   a timeout consumes nothing = #841-safe) and disarmed once mid-frame (since
   2026-10-06 the kthread instead reads only over a ready stream and parks on
   readiness hooks, which never block at a boundary: the amendment under
   item 2). This keeps
   the kthread lifetime simple (a stop-flag + join, **no Proc-lifecycle
   entanglement**) — rejected: an owning-Proc member thread (io_uring-faithful but
   new kernel-thread-reaping territory on the deepest-stakes surface) and a
   submission-only SQPOLL (half-delivers — completions would still need an `ENTER`).

2. **Completion waiting.** A **CQ wait-list** on the `struct Loom`: a thread in
   `SYS_LOOM_ENTER` (`min_complete >= 1`) that finds the CQ short of its target
   registers a `poll_waiter` on the list and sleeps; `loom_async_complete`, after
   `loom_post_cqe`, walks the list and wakes. This is the multi-waiter
   coordination Loom-3 deferred (Loom-3's wait drove the reader inline and a
   concurrent `ENTER` "returned what's posted"); it serves both the SQPOLL ring
   (the kthread is the reader, the `ENTER` caller sleeps) and the non-SQPOLL
   multi-waiter case (one `ENTER` drives the reader, peers sleep on the list).

   **As-built amendment (2026-09-30).** On a shared client the thread holding
   the reader role may belong to another Proc: a sync op in `client_wait` reads
   only until its own reply arrives, and its departing handoff designates only
   sync ops, so an async reply that lands after it leaves had no reader while
   the `ENTER` slept. An `ENTER` whose pump finds the role held therefore also
   registers on the client's **role-waiter list** (register-then-observe under
   `c->lock`), sharing its sleep's Rendez with the CQ hook. A handoff that leaves
   the role free and undesignated wakes that list, as does session death, and
   the woken `ENTER` pumps again. The SQPOLL kthread and the dev9p poll pump did
   not sleep on a busy role: they yielded and retried (until the amendment
   below, which gives all three waiters the same hooks).

   **Amendment (2026-10-06, operator vote "waiters fan in"; OPEN-BUGS
   2026-10-05 07:52Z + 18:56Z).** The fix above hooked ONE client: the client
   of the ring's newest in-flight op (`loom_first_inflight_client`). A ring's
   ops can span 9P clients -- an event loop over a socket and files is the
   canonical use -- and nothing reads a client's replies but its role holder, so
   a reply on any other client stayed unread while the picked one was held or
   slow (a parked socket read never answers). The picked client's pump also
   blocked in the recv whether or not anything was due, so an `ENTER` that
   pumped after another reader took its reply blocked blind. A waiter now
   **reads for every client it waits on, and only over a ready stream**:

   - It **scans** every client with an op in flight, from a rotating start,
     and pumps one whose role is free AND whose transport is **ready** -- bytes,
     or the EOF, at a frame boundary (`p9_client_reader_pump_ready`). Only the
     role holder consumes the stream, so the bytes it saw stay until it reads
     them: a waiter never blocks in a recv with nothing due.
   - With nothing to pump it **hooks** every such client, under `c->lock`
     (`p9_client_reader_hook`): a HELD role on the client's role-waiter list
     (the holder reads whatever arrives; its handoff wakes the list when it
     leaves the role free and undesignated); a FREE role with nothing to read
     on the transport's **readiness list** (every arrival wakes it); a free role
     over ready bytes ends the hooking and the scan runs again. One hook per
     client, never both lists: a held role hooked on readiness would miss the
     holder departing over a frame that has already arrived.
   - It hooks the CQ list as before and sleeps on one Rendez over all the
     hooks, `poll.c`'s one-flag-per-hook shape. A dead client ends nothing but
     its own part of the scan: its death posted error CQEs for its ops. Only
     the waiter's own death ends the wait.

   The three waiters run the same fan-in -- the non-SQPOLL `ENTER`, the SQPOLL
   kthread and the dev9p poll pump (NET-DESIGN 12.2) -- and differ only in what
   ends the wait. Precedent: Plan 9's `devmnt` (whoever waits reads), Fuchsia's
   port (a waiter fans in object readiness), io_uring's `DEFER_TASKRUN` (the
   completion work runs when the task waits). Rejected: a per-client async
   reader kthread (Linux `trans_fd`'s read worker), a new kthread lifecycle on
   the deepest surface for completions this design does not promise without an
   `ENTER`; and one client per ring, a restriction io_uring does not have.
   Modeled in `specs/loom_role.tla` (generalised to N clients; `NoMissedWake`,
   `NoBlindRecv`, `EnterReturns` with a deferred client).

**The new primitive.** A mandatory transport-vtable op `recv_ready(ctx, pw)`:
"a recv would not block at a frame boundary" (bytes, or the EOF), with `pw`
registered on the backend's readiness list in the same critical section as
the sample when it is non-NULL (srvconn: `s2c` bytes or EOF, its `poll_list`;
the pipe transport: the rx pipe's poll; the loopback test transports: a list
woken where a reply is queued). It is called under `c->lock`, which already
orders before every backend lock (the death hangup runs there).

*Amended 2026-10-06 (the loom-mc self-audit, S-3):* the pump reads with a
second mandatory op, `recv_now(ctx, buf, cap)` -- what is waiting, never
sleeping, `P9_TRANSPORT_EAGAIN` when nothing is (srvconn: `s2c` without its
parks; the pipe transport: `pipe_read_now`, the read end's `O_NONBLOCK` left
to EL0; the test transports: an empty queue). The bytes of a frame being read
belong to the CLIENT (`rx_got`), as Plan 9's devmnt keeps them in the mount's
queue and Linux's `trans_fd` in the connection: a pump that finds only part of
a frame leaves it there and returns IDLE, and the next reader resumes. So no
pump ever waits on a server. This was owed the moment the deadline gate went:
the one dev9p poll kthread now reads every QTPOLL session, pipe-served ones
included, and any process can serve a 9P mount over pipes and keep the read
end -- a server that stopped inside a frame, or a holder that took the bytes a
readiness sample saw, would have held that kthread, and with it every poller
in the system. The blocking readers (a sync op's election, the send path's
self-pump) still finish a frame they are inside before a stop or a death
unwinds them (#90, I-9), resumed frames included; that bound stays the
trusted-server one (the vault's `seam-90-hung-server`).

*Superseded 2026-10-06:* the NULL-permitted `set_recv_deadline` /
`recv_timed_out` ops and the deadline-aware pump
(`p9_client_reader_pump_once_deadline`) that woke the kthread at a frame
boundary every 10 ms (the dev9p poll pump every 20 ms per client), and the
register gate that refused an SQPOLL ring a transport without a deadline. A
waiter that reads only over a ready stream never blocks at a boundary, so the
deadline has nothing left to bound and is deleted; a pipe-attached mount
(`SYS_ATTACH_9P`) may now back an SQPOLL ring.

**The poll-thread loop** (`loom_sqpoll_main`):

```
loop:
  if stopping: exit
  sample sq_tail, cq_head, drive_gen  (what the in-flight park must notice)
  drain SQ -> loom_submit_one      (zero-syscall submit; NOP inline, FSYNC async)
  re-arm MORE-pending multishot ops; admit unblocked chain ops
  if async_inflight > 0:
     scan: pump_ready each in-flight client
       a frame read  -> loom_async_complete posted a CQE + woke the CQ
                        wait-list; loop
       nothing ready -> hook every client (role or readiness) and the CQ
                        list; if drive_gen is unmoved and ops are still in
                        flight, set LOOM_RING_SQ_NEED_WAKEUP and park on the
                        kthread's Rendez until a hook flag, the CQ flag,
                        sq_tail or cq_head moved since the sample, or stop
  else:
     set LOOM_RING_SQ_NEED_WAKEUP; park on the kthread's Rendez until work
       the CQ has room for (an SQE, a held re-arm or chain op), or stop (an
       ENTER wake-up re-checks)
```

**Two parks, two conditions (as built 2026-10-06).** The idle park's condition
reads the CQ without the ring lock and admits an SQE when the posted CQEs
leave room. Both hold only while nothing is in flight: with ops in flight a
completion on another CPU posts concurrently, and the CQ slots reserved for
ops in flight can refuse an SQE that a posted-only count admits, so the
kthread would wake, fail to admit, and wake again. The in-flight park wakes
instead on what moves: a hook flag (a client can be pumped), the CQ flag (a
completion another thread read), an SQE produced or a CQE reaped since the
loop top, or stop. Each is somebody's event, so a ring whose CQ cannot admit
does not spin.

**`drive_gen` (as built 2026-10-06).** A completion that another thread reads
posts its CQE before it records what the completion changes for the ring's
driver: a multishot op's re-arm, a chain successor's gate. A waiter woken by
that CQE can re-check before the record, find nothing to re-arm or admit, and
sleep with nothing left to wake it. A sibling thread's submit can likewise put
an op on a client the waiter collected before it, between the waiter's client
hooks and its CQ hook. Each ring keeps a generation, `drive_gen`, bumped under
`l->lock` by the CQE post, by the completion's state update, and by every op
that goes in flight (a submit's link, a re-arm claimed). The `ENTER`'s sleep and the kthread's in-flight park sample it at the
loop top and sleep only if it has not moved, re-reading it under `l->lock`
after the CQ hook is filed: a completion after that read flags the hook. The
completion half predates the fan-in (OPEN-BUGS 2026-10-06 16:20Z); its window
needs a completer on another CPU between its post and its record, and no test
reproduces it deterministically. The submit half is the fan-in's own, and a
test stalls the waiter in that window (`9p_client.loom_enter_sees_a_sibling_submit`).

**Lifetime.** The Loom owns the kthread; `loom_free` sets `stopping`, wakes the
park Rendez, and **joins** the kthread before freeing the ring (the kthread only
ever touches the still-allocated `struct Loom`). The kthread never sleeps in a
recv (it reads with `recv_now`), so the wake always reaches it. A CQ waiter holds a loom ref for its
`ENTER`, so the ring cannot free under a live waiter; teardown / session death
wakes the wait-list so no waiter strands.

**`SYS_LOOM_ENTER` on an SQPOLL ring** does **not** submit (the kthread owns
submission); it wakes the idled kthread (clearing `LOOM_RING_SQ_NEED_WAKEUP`) and,
if `min_complete > 0`, sleeps on the CQ wait-list (death-interruptible, #811)
until the target is met or nothing more can complete.

**Invariant.** The CQ wait-list adds **I-9 specialized to the CQ wait-list** —
no wakeup lost between a waiter's CQ check and its sleep (`CqFlagTracksCq` +
`NoMissedCqWake`) and no waiter stranded past teardown (`NoStrandedWaiter`),
modeled in `specs/loom.tla` (the register-then-observe `poll.tla` lineage; the
`BUGGY_CQWAIT_CHECK_EARLY` + `BUGGY_CQWAIT_NO_WAKE` counterexamples). The SQPOLL
kthread's submit + reader-drive reuse the audited Consume / Dispatch /
ReplyArrives / PostCqe path; only the wait/wake is the new modeled surface.

---

## 9. Invariants + audit-trigger surface

Loom reserves two ARCH §28 invariants (the table edit lands *with* impl, the way
Lazarus defers its §28 edit to W1):

- **I-29 — Loom completion integrity.** Every *submitted* SQE produces *exactly
  one* terminal CQE (no lost, no double); no CQE is posted whose `user_data`
  correlation is stale (an abandoned/torn-down op never surfaces as a live
  completion). Modeled in `specs/loom.tla`; GENERALIZED to a stream (one SQE ->
  many CQEs, exactly one terminal) in `specs/loom_multishot.tla`, and to a
  cancellation-complete dependency chain (a cancelled linked op is never silently
  dropped) in `specs/loom_order.tla` (Loom-5).
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
- **Loom-4 — SQPOLL** (§8.6; design Option 1, user-voted 2026-06-07): the
  `kproc()` poll-thread (zero-syscall hot path; `cpu_pinned`-able) + the
  frame-boundary idle-deadline (the #841-safe interruptible recv: a new
  NULL-permitted transport `set_recv_deadline` + a deadline-aware reader pump) +
  the CQ wait-list (an `ENTER` waiter sleeps for `min_complete`, woken by a posted
  CQE / teardown — the multi-waiter coordination Loom-3 deferred). `loom.tla`
  extended FIRST with the CQ-waiter (I-9: `CqFlagTracksCq` + `NoMissedCqWake` +
  `NoStrandedWaiter`; +2 buggy cfgs). Wait/wake + kthread-lifetime surface. Audit.
- **Loom-5 — the multishot MECHANISM + linked ops**: one SQE → many CQEs +
  LINK/DRAIN per-fid ordering. Multishot is built as a generic op-property
  (re-arm-after-each-completion; `LOOM_CQE_MORE` on every non-terminal shot,
  cleared on the terminal; the I-30 pin held across all shots + released once;
  CQ back-pressure HOLDS a shot when full; cancel/error yields exactly one
  terminal CQE — the I-29 generalization to a stream). Its real consumers (the
  Tapestry event-fd stream and the `/srv` accept-loop — *the same mechanism*) are
  payload ops that light up at Loom-6, so the mechanism is modeled (two NEW
  focused modules `specs/loom_multishot.tla` + `specs/loom_order.tla`, leaving the
  audited `specs/loom.tla` untouched — its single-CQE-per-op `cq` is gate-tied and
  cannot represent multishot's multiset CQ) + built + audited at Loom-5
  against **synthetic NOP/FSYNC multishot vehicles** (no real re-armable 9P op
  exists until Loom-6's payload surface; user-voted 2026-06-07 "mechanism at
  Loom-5, real ops at Loom-6"). LINK/DRAIN's real dependent ops (walk→open→read,
  write→read same fid) likewise light up at Loom-6. Audit.
- **Loom-6 — registered buffers + payload ops + native API + bench**: pinned
  Burrow regions (zero-copy payload) + the real payload-op dispatch (the event-fd
  multishot `LOOM_OP_READ` + the present `LOOM_OP_WRITE` + the READ/WRITE/WALK/…
  surface) the Loom-5 mechanism rides + the libthyla-rs Loom wrapper (the native
  userspace API — the backend the libtapestry `Loom` seam trait targets:
  `impl tapestry::Loom for libthyla_rs::loom::Ring`) + a latency benchmark on a
  high-fanout workload (the Tapestry present+input+vsync interactive loop; also
  globbing / many-connection server) showing the trap-amortization win. The
  `LOOM_OP_WIRE_PASSTHROUGH` seam stays reserved (designed, not built). Final
  audit + arc close.

### Loom's first consumer: Tapestry (graphics)

The concrete consumer that shapes Loom-5/6 is the **graphics fast-path**
(`docs/TAPESTRY.md`, signed off 2026-06-07): a native client (`libtapestry`)
operates a Loom ring against a display server (`tapestryd`, the stratumd-as-driver
pattern) to present framebuffer surfaces. It needs **zero new Loom core** — the
proof of Loom's generality:

- **present** = `LOOM_OP_WRITE` of a rect descriptor to a present-fid (no
  `LOOM_OP_PRESENT`; the opcode set stays pure 9P). The framebuffer is a separate
  `tapestryd`-owned Burrow the host DMA-reads out of band — **not** a Loom
  payload buffer — so present imposes no large-regbuf requirement.
- **input + vsync** = a **multishot `LOOM_OP_READ`** on an event-fid (Loom-5's
  mechanism, Loom-6's real op): arm once, a CQE per event forever.
- the present CQE is the **buffer-recycle gate**; Vsync is a separate event for
  pacing (triple-buffered, back-pressured — never op-cancellation).

So **present / input / audio are all the same shape — a 9P server + multishot +
registered buffers — capability-safe across the untrusted-app → trusted-server
boundary by I-29/I-30.** Graphics is therefore the canonical Loom-5/6 **benchmark
workload** (the API is shaped to fit the present+input+vsync loop), and it adds
one graphics-layer invariant — **T-1, no torn scanout** (a present's framebuffer
pages stay backed from submit to its terminal CQE; the #847 dual-refcount + #898
quiesce are the mechanism) — audited when the post-Loom graphics phase
(virtio-gpu scanout + `tapestryd`) lands, not in the Loom arc itself. See
`docs/TAPESTRY.md` §7 for the full requirements graph.

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
