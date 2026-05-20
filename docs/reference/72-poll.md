# 72 — poll: the multi-fd wait/wake primitive

**Status**: as-built at **P5-poll-a**. The poll mechanism
(`kernel/include/thylacine/poll.h`, `kernel/poll.c`), the `SYS_POLL`
syscall (syscall 29), the `Dev.poll` vtable slot, and `devpipe`'s real
`.poll` implementation all landed at P5-poll-a. The `devsrv` connection
and listener `.poll` lands at P5-poll-b; the formal audit closes at
P5-poll-b's #538 round.

---

## Purpose

`poll(fds, nfds, timeout_ms)` parks the caller until at least one of
`nfds` file descriptors is ready, or `timeout_ms` elapses. It is the
multi-source wait Thylacine had no other primitive for — `Rendez` is
strictly single-waiter (`<thylacine/rendez.h>` extincts on a second
waiter), so a thread cannot block on two readiness sources by waiting
on two `Rendez`es. poll closes that gap.

The v1.0 consumer is **`corvus`**, the key agent: a single-threaded
9P server reached via `/srv/corvus` (CORVUS-DESIGN.md §6.2). corvus
calls `poll` on its `/srv` listener handle plus every open connection
endpoint; when any becomes readable, it dispatches to the corresponding
9P-server step. Beyond corvus, `poll` is required by every Utopia-class
program (bash, musl libc, curl, Python asyncio) and is the wait
primitive the future Linux-compat shim binds 1:1.

This layer sits between `specs/poll.tla` (the binding model) and the
per-Dev readiness mutators (`kernel/pipe.c` at v1.0, `kernel/devsrv.c`
at P5-poll-b).

---

## Public API

All declarations are in `<thylacine/poll.h>`.

### `struct pollfd` — the userspace ABI

```c
struct pollfd {
    s32 fd;            // handle index (hidx_t); negative ⇒ POLLNVAL
    s16 events;        // requested events bitmask
    s16 revents;       // returned events bitmask (kernel-filled)
};
_Static_assert(sizeof(struct pollfd) == 8, ...);
```

8 bytes; field offsets pinned by `_Static_assert`. Linux-shaped so the
future musl shim is layout-identity.

### Event bits

```c
#define POLLIN     0x001    // data may be read without blocking
#define POLLOUT    0x004    // writing will not block
#define POLLERR    0x008    // error condition (output-only)
#define POLLHUP    0x010    // hang up — peer closed (output-only)
#define POLLNVAL   0x020    // fd not open / invalid handle (output-only)
```

Linux values. `events` may set `POLLIN`/`POLLOUT`;
`POLLERR`/`POLLHUP`/`POLLNVAL` are output-only — the kernel sets them
on `revents` regardless of `events`.

### The `Dev.poll` vtable op

Added to `struct Dev` in `<thylacine/dev.h>` between `bwrite` and
`remove`. The single Thylacine addition to the Plan 9 vtable (ARCH §9.2).

```c
short (*poll)(struct Spoor *c, short events, struct poll_waiter *pw);
```

- **Return value**: the currently-ready `revents` for `c`. POLLIN/
  POLLOUT filtered by `events`; POLLERR/POLLHUP unconditional when
  applicable.
- **`pw == NULL`**: sample-only. No list mutation. Used by `poll`'s
  post-wake re-scan.
- **`pw != NULL`**: register the hook on `c`'s `poll_waiter_list`
  atomically with the readiness sample, under `c`'s object lock — the
  load-bearing **register-then-observe** step.
- **NULL slot**: a Dev that does not implement `.poll` has the slot
  NULL; `poll` then treats the fd as always-ready for the requested
  `POLL_REQUESTABLE` (POLLIN | POLLOUT) bits. POSIX-correct for a
  regular file.

At v1.0 P5-poll-a only `devpipe` implements a real `.poll`. The other
ten Devs (devnone, devcons, devnull, devzero, devrandom, devproc,
devctl, devramfs, devsrv, dev9p) leave it NULL (always-ready);
devsrv's real `.poll` lands at P5-poll-b.

### The `poll_waiter` hook

```c
struct poll_waiter {
    u32                       magic;   // POLL_WAITER_MAGIC = "POLW"
    bool                      ready;
    struct Rendez            *rendez;  // poller's private Rendez
    struct poll_waiter_list  *list;    // non-NULL while listed
    struct poll_waiter       *next;
};
```

Stack-allocated by `sys_poll_for_proc` — one per pollfd, `nfds ≤
PROC_HANDLE_MAX = 64`. Hook lifetime is one `poll` call; the sweep at
the end of `sys_poll_for_proc` unregisters every still-listed hook so
no stack pointer outlives the call (`specs/poll.tla` `NoStaleHook`).

### The `poll_waiter_list` per-object list

```c
struct poll_waiter_list {
    spin_lock_t         lock;
    struct poll_waiter *head;
};
void poll_waiter_list_init(struct poll_waiter_list *l);
void poll_waiter_list_register(struct poll_waiter_list *l,
                               struct poll_waiter *pw);
void poll_waiter_list_unregister(struct poll_waiter *pw);
void poll_waiter_list_wake(struct poll_waiter_list *l);
```

A pollable object embeds one of these. `struct pipe_ring` carries one
at offset 72 (v1.0); `struct SrvService` / `struct SrvConn` will
embed one at P5-poll-b. The list lock is internal to the list — it is
NOT the object's existing lock. Lock order globally: **object lock →
list lock → rendez lock** (acquire chain in register and in wake;
unregister takes only list lock).

### The testable core

```c
s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds,
                      u64 nfds, s32 timeout_ms);
```

Operates on a kernel-side `kfds[]`. Returns:
- `≥ 0`: the number of pollfds with `revents != 0`.
- `-1`: error (nfds out of range, p NULL, kfds NULL).

The SYS_POLL user-VA wrapper (`sys_poll_handler` in `kernel/syscall.c`)
copies the array in, calls `sys_poll_for_proc`, then writes back only
the `revents` field of each pollfd; a partial-write fault scrubs the
already-written revents bytes to zero so userspace can never read a
torn revents.

### Diagnostics

```c
u64 poll_total_calls(void);  // monotonic — every poll call increments
u64 poll_total_slept(void);  // monotonic — increments only on tsleep path
```

The two together distinguish fast-path (immediate readiness) from
slow-path (sleep + wake) coverage in tests.

---

## Implementation

### The register-then-observe discipline

Quoting `specs/poll.tla`'s `Register` action and the spec preamble:

> `dev->poll` installs the hook AND samples readiness in ONE locked
> step under the object's own lock. No readiness event between the
> sample and the sleep can reach a still-empty hook list.

`devpipe_poll` (`kernel/pipe.c:206-219`) is the canonical
implementation:

```c
static short devpipe_poll(struct Spoor *c, short events,
                          struct poll_waiter *pw) {
    struct pipe_endpoint *p = priv_of(c);
    if (!p || !p->ring) return POLLERR;
    struct pipe_ring *r = p->ring;

    spin_lock(&r->lock);
    short revents = devpipe_revents_under_lock(r, p, events);
    if (pw) {
        poll_waiter_list_register(&r->poll_list, pw);
    }
    spin_unlock(&r->lock);
    return revents;
}
```

A producer mutating `r->count` / `r->read_eof` / `r->write_eof` takes
`r->lock`. The producer's wake site is therefore mutually exclusive
with `devpipe_poll`'s sample. The case the discipline prevents: a
producer fires between the sample and the install — that's
unreachable because both happen under `r->lock`.

### The producer-side wake

Each existing wakeup site in `kernel/pipe.c` (line 231 close, 285
read-drains, 328 write-appends) also calls `poll_waiter_list_wake`
right after releasing `r->lock`:

```c
spin_unlock(&r->lock);
wakeup(&r->read_rendez);
poll_waiter_list_wake(&r->poll_list);  // sets pw->ready + signals each rendez
```

The wake walks under the list lock; for each registered hook it (a)
sets `pw->ready = true`, then (b) calls `wakeup(pw->rendez)`. The
order is load-bearing: setting `ready` BEFORE `wakeup` ensures the
post-resume cond re-check sees `ready=true` via the rendez-lock
release/acquire pair (see `<thylacine/poll.h>` preamble).

### The poll syscall flow

`sys_poll_for_proc` (`kernel/poll.c:208-277`) is:

1. **Validate args** — nfds in `[1, PROC_HANDLE_MAX]`, kfds non-NULL,
   p non-NULL.
2. **Stack-allocate** the poller's private `Rendez r` and
   `struct poll_waiter waiters[PROC_HANDLE_MAX]`. `poll_waiter_init`
   each (clears flags, sets back-ref to `&r`).
3. **First scan**: for each pollfd, call `dev->poll(spoor, events,
   &waiters[i])` — register + sample atomically. Accumulate
   `ready_count`.
4. **Fast path**: if `ready_count > 0` OR `timeout_ms == 0`, jump
   straight to the unregister sweep + return.
5. **Slow path**: compute `deadline_ns` from `timeout_ms`:
   - `< 0` → 0 (no deadline; infinite wait).
   - `> 0` → `timer_now_ns() + timeout_ms * 1_000_000`; clamp on u64
     overflow; nudge a zero result to 1 (the tsleep "no deadline"
     sentinel reads 0).
6. **tsleep** on the poller's private `r` with cond
   `poll_cond_any_flagged` (returns 1 iff any `waiters[i].ready` set).
   The cond runs under `r`'s lock; releases-and-acquires on the list
   lock and the rendez lock provide the cross-lock visibility.
7. **Re-scan**: call `dev->poll(spoor, events, NULL)` per fd —
   sample-only, no register. Accumulate the new `ready_count`. (A
   pure `TSLEEP_TIMEDOUT` resume sees `ready_count == 0`; a wake
   resume sees ≥ 1.)
8. **Sweep**: `poll_waiter_list_unregister(&waiters[i])` per hook —
   idempotent on already-unregistered hooks; scribbles `pw->magic = 0`
   on each as a defense-in-depth UAF guard.

### Lock ordering

Globally: **object lock → list lock → rendez lock**. Concretely:

- `devpipe_poll` (register path): `r->lock` → `poll_list->lock` (via
  `poll_waiter_list_register`).
- Producer wake: `r->lock` (release) → `poll_list->lock` (via
  `poll_waiter_list_wake`) → `rendez->lock` (via `wakeup` per hook).
- `sys_poll_for_proc` sweep: `poll_list->lock` only (via
  `poll_waiter_list_unregister`).
- `tsleep` resume: `rendez->lock` only; the cond reads `pw->ready`
  WITHOUT object lock — sound by the lock-release/acquire pair on
  the list lock that produces it (see `<thylacine/poll.h>`).

No path takes locks in inverted order. The unregister sweep
deliberately takes ONLY the list lock so it can run after the syscall
has decided to return — no deadlock with a concurrent producer that
holds the object lock.

---

## Data structures

### `struct pipe_ring` layout (v1.0 P5-poll-a, 88-byte header)

```
offset  0:  u32  magic
offset  4:  int  ref            (atomic; 2 at create; per-endpoint close -1)
offset  8:  size_t count
offset 16:  size_t head
offset 24:  size_t tail
offset 32:  bool read_eof
offset 33:  bool write_eof
offset 34-35: pad
offset 36:  spin_lock_t lock    (4 B; u32)
offset 40:  struct Rendez read_rendez   (16 B: spin_lock + pad + Thread*)
offset 56:  struct Rendez write_rendez  (16 B)
offset 72:  struct poll_waiter_list poll_list  (16 B: spin_lock + pad + ptr)  ← NEW at P5-poll-a
offset 88:  u8 buf[PIPE_BUF_SIZE]
```

`_Static_assert(sizeof(struct pipe_ring) == 88 + PIPE_BUF_SIZE)` pins
the layout.

### `struct pollfd` ABI

8 bytes; pinned by `_Static_assert` at the type and at every offset
(fd=0, events=4, revents=6).

---

## State machines

### The `poll` lifecycle

PC states from `specs/poll.tla` mapped to source positions in
`kernel/poll.c::sys_poll_for_proc`:

| spec PC | Source position | What |
|---|---|---|
| `start` | function entry | poll() called, no hook installed |
| `registered` | after first-scan loop, lines 229-237 | every hook on every fd; sample captured into `waiters[].ready` |
| `done_ready` (fast) | `goto unregister_and_return` at line 244 | `ready_count > 0`, return immediately |
| `done_timeout` (fast non-blocking) | same goto at line 248 | `timeout_ms == 0`, no scan-and-go |
| `sleeping` | `tsleep` call at line 262 | parked on the poller's private rendez |
| `registered` (resume) | post-tsleep re-scan, lines 266-274 | woken by cond or deadline; re-sample everyone |
| `done_ready` / `done_timeout` | sweep at lines 271-275 + return | sweep unhooks + scribbles magic, then returns |

### Hook lifecycle

```
INIT (poll_waiter_init: magic = MAGIC, ready = FALSE, list = NULL, next = NULL)
  |
  | poll_waiter_list_register (under list lock; pw->list = l; prepend to head)
  v
LISTED (pw->list non-NULL; visible to producer walks)
  |
  | Producer: pw->ready = TRUE; wakeup(pw->rendez)   (under list lock)
  v
LISTED + FLAGGED (pw->ready TRUE; still on list)
  |
  | poll_waiter_list_unregister (under list lock; pw->list = NULL; remove)
  v
UNLISTED (pw->list NULL; ready field retained but list-detached)
  |
  | (sweep scrubs pw->magic = 0; the stack frame holding pw pops)
  v
DEAD
```

The DEAD scrub is defense-in-depth: a stale hook walked by a producer
must extinct in `poll_waiter_list_wake`'s magic check rather than
silently corrupt.

---

## Spec cross-reference

The canonical mapping lives in `specs/SPEC-TO-CODE.md` `poll.tla`
section. Summary:

| spec action | code location |
|---|---|
| `Register` | `kernel/poll.c:229-237` (first scan); `kernel/pipe.c:206-219` (`devpipe_poll`) |
| `CommitOrSleep` | `kernel/poll.c:239-265` |
| `MakeReady(f)` | `kernel/pipe.c:231` (close), `:285` (read), `:328` (write); devsrv at P5-poll-b |
| `Timeout` | `kernel/sched.c::tsleep` (composed; `tsleep.tla`) |
| `NoStaleHook` | `kernel/poll.c:271-275` (unregister sweep + magic scrub) |

---

## Tests

`kernel/test/test_poll.c` — 12 tests, all PASS × default + UBSan:

| Test | What |
|---|---|
| `poll.ready_immediately_pollin` | Pre-write a byte; poll returns 1 with POLLIN immediately, no sleep. |
| `poll.ready_immediately_pollout` | Empty pipe; poll(write end, POLLOUT) returns 1 immediately. |
| `poll.timeout_zero_not_ready` | Empty pipe + timeout=0 → return 0 immediately; never enters tsleep. |
| `poll.timeout_positive_fires` | Empty pipe + timeout=10ms → tsleep fires; return 0. |
| `poll.block_then_wake_pollin` | Consumer thread polls(timeout=-1) on empty pipe → SLEEPING; boot writes → consumer wakes with POLLIN. The headline slow-path test. |
| `poll.pollhup_on_close_write_end` | Consumer polls; boot `handle_close`s the write end → consumer wakes with POLLHUP. |
| `poll.multi_fd_one_ready` | 3 pipes, one has data; poll all 3 returns 1 with the correct pollfd revents. The headline N-fd test. |
| `poll.bad_fd_revents_pollnval` | Invalid hidx (unallocated / out-of-range) → revents = POLLNVAL, count includes the bad fd. |
| `poll.bad_args_rejected` | nfds==0, nfds>PROC_HANDLE_MAX, p==NULL, kfds==NULL → -1. |
| `poll.always_ready_null_dev_poll` | devnull Spoor (NULL .poll slot) → revents = POLLIN\|POLLOUT immediately. |
| `poll.pollerr_on_write_after_read_close` | Read end closed; write end's revents includes POLLERR. |
| `poll.unregister_after_fast_path` | Two consecutive polls on the same fd succeed identically — verifies the fast-path sweep unregistered the first call's hooks. |

What the suite **explicitly does NOT cover at v1.0**:

- `devsrv` poll (lands with the P5-poll-b test set).
- 9P-mounted-file readiness (deferred — synthetic-9P readiness
  notification is a separable follow-on).
- `select()` (deferred — implemented on top of `poll`).
- `epoll_*` (deferred to v1.1).

---

## Error paths

`sys_poll_for_proc` (the testable core):

- `p == NULL` → `-1`.
- `nfds == 0` → `-1` (no fds is not a valid `poll` call).
- `nfds > PROC_HANDLE_MAX` → `-1` (handle-table bound).
- `kfds == NULL` → `-1`.

Per-pollfd (`revents` filled; the call as a whole still returns a
non-negative count):

- `fd < 0` → `revents |= POLLNVAL`.
- `handle_get(p, fd) == NULL` (out-of-range or unallocated slot) →
  `revents |= POLLNVAL`.
- `slot->kind != KOBJ_SPOOR` → `revents |= POLLNVAL` (poll is fd-shaped;
  non-Spoor handles have no readiness semantics).

`sys_poll_handler` (the user-VA wrapper):

- `nfds_raw == 0 || > PROC_HANDLE_MAX` → `-1`.
- `fds_va` outside `[0, UACCESS_USER_VA_TOP - nfds*8]` → `-1`.
- Partial-write fault on revents writeback → already-written revents
  bytes scrubbed to 0; return `-1`. The torn-write defence mirrors
  `sys_srv_peer_handler`.

---

## Performance characteristics

Not yet benchmarked. Expected costs:

- **First scan**: O(nfds) calls to `dev->poll`, each one spin_lock /
  unlock on the polled object's lock plus one list_register (another
  spin_lock pair on the list). Each pollfd dominated by the lock pair.
- **Cond evaluation under tsleep**: O(nfds) over the local
  `waiters[]` array per wakeup. Re-evaluated once per wake.
- **Wake walk** (producer side): O(N_pollers_on_this_fd) under the
  list lock. Each walk: per-hook `wakeup` is one rendez-lock pair.

There is no allocation in the hot path — `waiters[]` is stack-resident
(`PROC_HANDLE_MAX = 64` × ~32 B = ~2 KiB).

---

## Status

Implemented today (P5-poll-a):
- `struct poll_waiter` + `struct poll_waiter_list` mechanism, with
  list-internal lock and the register/unregister/wake operations.
- `Dev.poll` vtable slot, between `bwrite` and `remove`.
- `sys_poll_for_proc` testable core + `sys_poll_handler` user-VA
  wrapper + the SYS_POLL = 29 dispatch case.
- `devpipe_poll` with POLLIN / POLLOUT / POLLHUP / POLLERR semantics
  and the wake callouts on every existing wakeup site
  (`devpipe_close`, `devpipe_read`, `devpipe_write`).
- `usr/lib/libthyla-rs::t_poll` + `TPollFd` libt stub.
- 12 tests in `kernel/test/test_poll.c`; **489/489 PASS × default +
  UBSan** (477 → 489 = 12 new poll tests).

Pending (P5-poll-b):
- `devsrv` connection Spoor's `.poll` (POLLIN when `c2s` has bytes,
  POLLHUP on teardown).
- `KObj_Srv` service handle's `.poll` (POLLIN when the accept queue
  is non-empty — corvus's listener).
- The P5-poll formal audit (`#538`).

---

## Known caveats / footguns

- **Stack frame size**. `sys_poll_for_proc` carries
  `struct poll_waiter waiters[PROC_HANDLE_MAX]` (~2 KiB) and a
  `Rendez` on its stack. Adding a second large array here without
  measuring kernel-stack headroom is a footgun. v1.0 kernel-thread
  stacks are 16 KiB so there is ample room, but `nfds`'s `64` bound is
  also the safety bound on `waiters[]`.
- **NULL `Dev.poll` ≡ always-ready** is the v1.0 semantics for every
  Dev that does not implement `.poll`. A future Dev that needs a
  *blocking* readiness (a pipe-like with real wait/wake) MUST
  implement `.poll` — leaving it NULL means polling on that fd will
  never block.
- **`POLLNVAL` is "ready"** in the POSIX sense: an invalid fd
  contributes 1 to the returned count. Callers iterating "the ready
  fds" must check `revents != 0`, not just look for POLLIN/POLLOUT.
- **No `EINTR` semantics yet** — the syscall doesn't translate a note
  delivery during the wait into an early return. Note plumbing isn't
  wired through `tsleep` at v1.0; `poll` blocks until cond OR
  deadline, never spuriously.
- **No fd ownership transfer** — `poll` does NOT take a reference on
  the polled Spoors. A concurrent `handle_close` on a fd whose poll
  is in flight: the close walks the Spoor's pipe-ring close path
  which fires `poll_waiter_list_wake`, so the poller wakes with
  POLLHUP/POLLERR. Sound. But a Spoor whose Dev was unregistered or
  whose backing object freed via some non-close path would be a UAF;
  no such path exists at v1.0.

---

## Naming rationale

`poll` is kept verbatim — POSIX, Linux, every Unix has used the name
for decades; renaming would obscure the surface for every user. The
internal `poll_waiter` / `poll_waiter_list` types are likewise direct
— they describe what they are. No thematic rename considered.

---

## See also

- `specs/poll.tla` — the binding TLA+ model. `Register`,
  `CommitOrSleep`, `MakeReady`, `Timeout`, `NoStaleHook` invariant.
- `specs/tsleep.tla` — the deadline-bounded `Rendez` sleep poll builds
  on. `poll`'s timeout IS a `tsleep` deadline.
- `specs/scheduler.tla` — the single-`Rendez` wait/wake atomicity
  that `tsleep` composes over.
- ARCH §23.3 — the SYS_POLL ABI and v1.0 scope boundary.
- ARCH §9.2 — the Dev vtable; `.poll` is the one Thylacine addition.
- ARCH §11.2 — the syscall row.
- ARCH §25.4 — `kernel/poll.c` is an audit-trigger surface.
- `docs/reference/16-rendez.md` (the `tsleep` section) — the
  deadline-bounded sleep poll consumes.
- `docs/reference/51-pipe.md` — the v1.0 .poll-implementing Dev.
- `docs/reference/70-devsrv.md` / `71-srvconn.md` — the P5-poll-b
  .poll-implementing Devs.
- CORVUS-DESIGN.md §6.2 — corvus's poll-on-N-connections single-thread
  server loop (the headline v1.0 consumer).
