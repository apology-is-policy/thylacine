# 61. /stratumd-stub — userspace 9P responder (P5-stratumd-stub-bringup-a)

The first Phase 5 test that runs **two userspace Procs cooperating over pipes**. `/stratumd-stub` is a minimal userspace 9P responder; `/attach-probe` is the client (reused from P5-attach-probe). The kernel test framework wires them via two pipe pairs and reaps both. Until this chunk, every userspace 9P test used a kernel-thread responder. This chunk proves a real userspace process can be the 9P server — the production shape for `stratumd-system`, `stratumd-michael`, etc.

This is the first of the P5-stratumd-stub-bringup arc. The full arc is:

| Sub-chunk | Scope | Status |
|---|---|---|
| **a** | Userspace responder + 2-userspace-Proc demo via kernel-supervised setup | LANDED (`479d997`) |
| **b** | `SYS_SPAWN_WITH_FDS` + `/stub-driver` userspace orchestrator (production shape, separate binary) | LANDED (`73784b4`) |
| **c** | Joey runs the `/stub-driver` orchestration inline on the production boot path | LANDED (`6c1c816`) |
| **d** | Stub serves Twalk + Tlopen + Tread over a synthetic FS (`/hello`) — `/stub-fs-probe` validates | LANDED (`cde54e8`) |
| **e1** | `SYS_WALK_OPEN` syscall + `t_walk_open` libt wrapper + `/stub-walk-probe` end-to-end via kernel 9P client; joey-inline content check on every boot | LANDED (`f37748b`) |
| **e2** | `SYS_CHROOT` syscall + `Territory.root_spoor` + `SYS_WALK_OPEN_FROM_ROOT` sentinel; v1.0 chroot mechanism per `CORVUS-DESIGN.md §10.1`; chroot path exercised via `/stub-walk-probe` (child Proc) + 6 dedicated kernel tests | LANDED (*(pending)*) |
| f (future) | Real stratumd swap-in (after Phase 6 musl sysroot) | DEFERRED |

---

## Purpose

A foundational integration step: prove a USERSPACE process can be the 9P responder. The pattern is exactly what `stratumd-system` will do at boot:

1. Hold two transport Spoors (read end + write end of pipes connected to the client).
2. Loop reading framed `T*` messages from the read end.
3. Build canonical `R*` responses (`Rversion`, `Rattach`, `Rclunk`, fallback `Rlerror`).
4. Write responses to the write end.
5. Exit cleanly on EOF.

If a userspace responder can drive this against the kernel's 9P client + dev9p + spoor-transport + pipe stack end-to-end, real `stratumd` can too — only the responder's internal logic differs (synthesised data vs Stratum's actual datasets).

---

## Userspace binary

`usr/stratumd-stub/stratumd-stub.c` (~170 LOC):

- Calling convention: kernel test framework pre-installs two `KOBJ_SPOOR` handles at fd 0 (`rx`, reads Tmsgs) + fd 1 (`tx`, writes Rmsgs).
- Reads 4-byte size prefix; if 0 bytes returned, EOF → exit 0.
- Reads `size - 4` more bytes to complete the frame.
- Dispatches on byte at offset 4 (`P9 type`):
  - `TVERSION` (100) → `RVERSION` with `msize=4096` + `"9P2000.L"`.
  - `TATTACH` (104) → `RATTACH` with root qid (`QTDIR`, version=0, path=1).
  - `TCLUNK` (120) → `RCLUNK` (empty body).
  - anything else → `RLERROR` with errno=5 (EIO).
- Loops until EOF or write-side failure.

Built via the standard userspace pipeline (CMake + libt link + static-PIE ELF). Cpio entry name: `stratumd-stub`.

### Wire-encoding helpers

Because `libt` has no libc, the binary inlines its own byte-ordering helpers:

```c
static void put_u16(unsigned char *p, unsigned short v);
static void put_u32(unsigned char *p, unsigned int v);

static long read_exact(long fd, unsigned char *buf, long n);
static int  write_all (long fd, const unsigned char *buf, long n);
```

`read_exact` returns `n` (full read), `0` (EOF before any byte), or `-1` (partial read after EOF or error). `write_all` loops over short writes.

### Why Tclunk handled even though the client never sends it

The v1.0 `/attach-probe` client sends only `Tversion + Tattach`. `Tclunk` on `root_fid` is rejected at the session layer (`kernel/9p_session.c::p9_session_send_clunk` line 303) — root fids are session-scoped, cleaned up implicitly at session close. So the wire never carries a Tclunk in this test.

`stratumd-stub` handles `Tclunk` anyway because:
- It costs nothing.
- It keeps the responder forward-compatible with clients that DO clunk non-root fids (future Walk-derived fids).
- It documents the response shape explicitly.

---

## Kernel test framework

`kernel/test/test_stratumd_stub.c` (~210 LOC):

1. `devramfs_lookup("stratumd-stub", ...)` → 8-aligned copy into `g_stub_blob`.
2. `devramfs_lookup("attach-probe", ...)` → 8-aligned copy into `g_client_blob`.
3. `pipe_create × 2` → 4 Spoors:
   - `c2s_rd` + `c2s_wr` — client-to-server.
   - `s2c_rd` + `s2c_wr` — server-to-client.
4. `rfork(RFPROC, stub_exec_thunk, ...)` for the stub Proc. Thunk:
   - `handle_alloc(KOBJ_SPOOR, c2s_rd)` → fd 0.
   - `handle_alloc(KOBJ_SPOOR, s2c_wr)` → fd 1.
   - `exec_setup` from `g_stub_blob`.
   - `userland_enter`.
5. `rfork` for the client Proc (`/attach-probe`). Thunk:
   - `handle_alloc(KOBJ_SPOOR, c2s_wr)` → fd 0.
   - `handle_alloc(KOBJ_SPOOR, s2c_rd)` → fd 1.
   - `exec_setup` from `g_client_blob`.
   - `userland_enter`.
6. `wait_pid × 2`. Collects both pids + statuses; asserts both 0.

Boot log on success:

```
    /stratumd-stub size=17824 + /attach-probe size=15144 — wiring 2 userspace Procs via 2 pipes
stratumd-stub: serving on fd 0 (rx) + fd 1 (tx)
attach-probe: PASS
stratumd-stub: EOF on rx; exit 0
    stratumd-stub pid=N1 status=0 + attach-probe pid=N2 status=0 — userspace 9P server end-to-end verified
```

The four interleaved lines (`stratumd-stub` startup, `attach-probe: PASS`, `stratumd-stub: EOF on rx`, framework summary) prove the two-userspace-Proc cooperation works end-to-end.

---

## Refcount discipline (load-bearing)

Same pattern as `test_attach_probe.c` (P5-attach-probe), extended to two userspace Procs:

- Each `pipe_create` Spoor's `ref=1` is **transferred** (not bumped) to its owner's handle table via `handle_alloc`.
- After both rfork thunks run, boot holds **zero** refs.
- Each Spoor's lone ref lives in one Proc's handle table.
- When the Proc exits, `handle_table_free` runs `spoor_clunk` on each fd. Last drop fires `devpipe_close`, which propagates EOF to the other side of the ring.

The 4 transfers:
- `c2s_rd` → stub's handle table fd 0.
- `s2c_wr` → stub's handle table fd 1.
- `c2s_wr` → client's handle table fd 0.
- `s2c_rd` → client's handle table fd 1.

Termination cascade:
1. `/attach-probe` finishes its sequence (Tversion + Tattach + close-attach + close-tx + close-rx + exit 0).
2. Client's `t_close(tx_fd)` drops `c2s_wr` to ref 0 → `devpipe_close` → `write_eof` on c2s ring.
3. `/stratumd-stub`'s next read on `c2s_rd` returns 0 (EOF). Stub prints `"EOF on rx; exit 0"`, exits.
4. Both Procs are zombies; framework reaps both via `wait_pid`.

---

## Why no RFFDG / SYS_SPAWN_WITH_FDS yet

This chunk is **kernel-supervised setup**: the kernel test framework pre-installs handles in each child's handle table via the rfork thunk, before exec runs. This works for a test but is NOT the production shape: production joey can't reach into a child's handle table from userspace.

The production shape needs:
- **RFFDG** in `rfork` (Plan 9 flag — copy the fd table at fork) so `joey` can fork a child that inherits its open Spoors.
- OR **`SYS_SPAWN_WITH_FDS`** — an explicit-inheritance variant of `SYS_SPAWN` that takes an fd-list to install in the child.

Either lands as a subsequent chunk. This chunk demonstrates the wiring works; the production-from-userspace path is the next architectural step.

---

## Buffer sizes

| Buffer | Size | Reason |
|---|---|---|
| `g_stub_blob` | 32 KiB (`STUB_BLOB_MAX`) | Holds the loaded stratumd-stub ELF (~17.8 KiB at this chunk; headroom for growth). |
| `g_client_blob` | 32 KiB | Holds attach-probe (~15.1 KiB). |
| Userspace `req`/`resp` | 256 bytes each | Max 9P frame in this test is `Rversion` (≤25 bytes) or `Rattach` (20 bytes). |

The two blobs are independent because both children may be executing concurrently; one shared buffer would race.

---

## Tests

| Test | Verifies |
|---|---|
| `userspace.stratumd_stub_round_trip` | Two userspace Procs cooperate end-to-end via two pipe pairs; both exit 0. |

Test count: 423 → 424 PASS × default + UBSan.

Earlier tests still covered (P5-attach-probe's `userspace.attach_probe_round_trip` uses the kernel-thread responder; this new test uses a userspace responder). The two coexist.

---

## P5-stratumd-stub-bringup-c — joey runs the orchestration inline

Sub-chunk **c** moved the `/stub-driver` orchestration from a standalone test binary onto joey's production boot path. After the corvus arc finishes (Q11 recovery verified, line `corvus-d hybrid-PKE round-trip verified via /srv/corvus (b3b)`), joey calls a new `do_stratumd_stub_bringup()` static (`usr/joey/joey.c`) that runs the same sequence `/stub-driver` does:

1. `t_pipe × 2` — four KOBJ_SPOOR fds (c2s + s2c rings, half-duplex).
2. `t_spawn_with_fds("stratumd-stub", 13, [c2s_rd, s2c_wr], 2)` — spawns the stub with the server-side ends pre-installed at its fd 0 (rx) and fd 1 (tx). Parent retains own holds.
3. `t_close(c2s_rd) + t_close(s2c_wr)` — drop joey-side refs on the stub's transport fds. Without this, the stub never sees EOF (joey would still be an alive reader/writer on the rings).
4. `t_attach_9p(c2s_wr, s2c_rd, "/", 1, 0)` — drive Tversion + Tattach over the byte-pipe pair.
5. `t_mount(attach_fd, 99, 0) + t_unmount(99)` — graft + ungraft at `target_path_id = 99` (matches `/stub-driver`'s convention to keep the kernel-test and boot-path numbers aligned).
6. `t_close(attach_fd) + t_close(c2s_wr) + t_close(s2c_rd)` — last drops; c2s_wr write_eof propagates so the stub's next read returns 0.
7. `t_wait_pid(&status)` — reaps the stub; asserts `status==0`.

Failure semantics: any sub-step prints a `joey: stub-bringup ... FAILED` diagnostic and returns -1; joey's main treats that as a boot regression and exits 1.

Boot log on success:

```
joey: corvus-d hybrid-PKE round-trip verified via /srv/corvus (b3b)
stratumd-stub: serving on fd 0 (rx) + fd 1 (tx)
stratumd-stub: EOF on rx; exit 0
joey: stub-bringup ok (pipe + spawn + attach + mount + unmount)
  joey: /joey pid=N exited cleanly (status=0)
Thylacine boot OK
```

Value-add over `/stub-driver` (b):
- Production-shape evidence on the actual boot path (not a separate test binary).
- Joey now exercises `SYS_ATTACH_9P` + `SYS_MOUNT` on every boot — any regression in those surfaces becomes a boot-time signal.
- The line that says "boot pivot is possible" moves from a self-contained kernel-test artifact onto the same boot path that runs corvus.

What it does **not** yet do (sub-chunks d/e):
- Reads through the mount — the stub returns Rlerror on Twalk, so `dev_walk` into the mount fails. Sub-chunk d adds Twalk + Tlopen + Tread to the stub.
- Doesn't make `/sysroot` joey's namespace root — that's sub-chunk e (territory-root pivot).

---

## P5-stratumd-stub-bringup-d — stub serves a synthetic FS

Sub-chunk **d** extends the stub from a Tversion/Tattach/Tclunk shell into a real (tiny) 9P file server. The stub now responds to:

- **Twalk** — walks from a fid to a new fid via 0-or-1 name components. `nwname=0` clones the source binding; `nwname=1, name="hello"` from root walks to `/hello`. Anything else → Rlerror ENOENT.
- **Tlopen** — marks a fid open. Flags ignored (synthetic FS is read-only).
- **Tread** — for `/hello` only: returns the requested slice of `HELLO_CONTENT = "hello from stratumd-stub\n"`. EOF returns `count=0` (NOT an error). Other paths → Rlerror EISDIR.
- **Tclunk** — extended to free the fid-table slot (idempotent for unbound fids).
- **Tattach** — extended to parse `fid` and bind it to the root.

Synthetic FS layout (compile-time fixed; paths double as qid path values):

| Path | qid path | qid type | Content |
|---|---|---|---|
| `/` | 1 | QTDIR (`0x80`) | (directory; readdir not implemented at this sub-chunk) |
| `/hello` | 2 | QTFILE (`0x00`) | `"hello from stratumd-stub\n"` (25 bytes) |

**Per-session fid table** (`usr/stratumd-stub/stratumd-stub.c`): 16-slot fixed array. Each entry: `{fid_id, path, opened}`. `path=0` means free. Helpers: `fid_find`, `fid_alloc`, `fid_free`. Tattach + Twalk allocate slots; Tclunk frees. Same-fid walks (`newfid == fid`) replace the binding in place per 9P semantics.

**Companion userspace binary `/stub-fs-probe`** (`usr/stub-fs-probe/stub-fs-probe.c`, ~280 LOC): the mirror-image client. Where `/stratumd-stub` responds to Tmsgs, `/stub-fs-probe` drives them. The kernel test framework pre-installs `c2s_wr` at probe's fd 0 (tx) and `s2c_rd` at fd 1 (rx). Probe sequence:

1. Tversion(NOTAG, msize=4096, "9P2000.L") → Rversion
2. Tattach(fid=0, afid=NOFID, uname="", aname="/", n_uname=0) → Rattach
3. Twalk(fid=0, newfid=1, nwname=1, ["hello"]) → Rwalk(nwqid=1) — assert qid type == QTFILE
4. Tlopen(fid=1, flags=0) → Rlopen
5. Tread(fid=1, offset=0, count=64) → Rread(count=25, data=HELLO_CONTENT) — assert content bytewise
6. Tread(fid=1, offset=25, count=64) → Rread(count=0) — EOF validation
7. Tclunk(fid=1) + Tclunk(fid=0)
8. `t_putstr("stub-fs-probe: PASS\n") + exit 0`

Each step that fails prints a `stub-fs-probe: <op> FAIL` diagnostic and exits 1.

### Why a raw-9P userspace client (not t_attach_9p + walk-through-mount)?

At this sub-chunk Thylacine has no userspace walk/open syscall that reaches into a dev9p-backed Spoor's underlying 9P fid table. Only `t_srv_connect` drives walk-and-open, and that's `/srv`-specific (the connection returns a pre-opened KObj_Srv fd, not a walked-9P fid). Driving raw bytes through pipes is the minimum-scope path to exercise the stub's Twalk / Tlopen / Tread handlers end-to-end without inventing a new syscall. Sub-chunk **e** (the territory-root pivot, which will need an analogous walk primitive for the mount-side path) is the natural place to land userspace walk-through-mount semantics.

### Kernel test wiring

`kernel/test/test_stratumd_stub.c::test_stratumd_stub_fs_round_trip` is structurally identical to the existing `stratumd_stub_round_trip` test, but the client is `/stub-fs-probe` instead of `/attach-probe`:

1. Load `/stratumd-stub` + `/stub-fs-probe` via `devramfs_lookup`.
2. `pipe_create × 2` → c2s + s2c rings.
3. `rfork × 2` with the existing `stub_exec_thunk` installing fds 0+1 in each child before `exec_setup`.
4. `wait_pid × 2`; assert both `status==0`.

The two tests coexist — they cover orthogonal slices of the same stub binary (handshake-only vs full walk/open/read).

### Test count

The new `userspace.stratumd_stub_fs_round_trip` brings total tests 525 → 526 PASS × default + UBSan.

---

## P5-stratumd-stub-bringup-e1 — walk-through-mount via `SYS_WALK_OPEN`

Sub-chunk **e1** lands the v1.0 minimum walk-through-mount primitive: a new syscall `SYS_WALK_OPEN(spoor_fd, name, omode) → opened_fd` that composes `spoor_clone` + `dev->walk` + `dev->open` + `handle_alloc` atomically. With this in hand, joey (and any test probe) can reach files served by a 9P server through the kernel 9P client + dev9p Dev vtable — no raw-wire bypass.

### Scope (minimum-surface)

Single-component walks only at v1.0:

- `name` must not contain `/`, `\0`, equal `.`, or equal `..` — multi-component path resolution and traversal land with the production `open(name, mode)` namec walker.
- `omode` is the Plan-9 low-2-bit envelope (`OREAD=0`, `OWRITE=1`, `ORDWR=2`, `OEXEC=3`) optionally OR'd with `OTRUNC=0x10`. Bits outside `SYS_WALK_OPEN_OMODE_VALID` are rejected.
- Dev-agnostic: any Dev with non-NULL `.walk` and `.open` works. v1.0 callers exercise dev9p (the attach root from `SYS_ATTACH_9P`). devramfs walks are kernel-internal at v1.0; nothing prevents userspace from calling `SYS_WALK_OPEN` on a devramfs Spoor when one becomes reachable.

### Kernel surface (`kernel/syscall.c::sys_walk_open_handler`)

```
SYS_WALK_OPEN(spoor_fd, name_va, name_len, omode) → fd or -1
```

1. Validate `name_len ∈ (0, 64]`, user-VA range, `omode` bit set.
2. Look up source Spoor (`KOBJ_SPOOR` + `RIGHT_READ`).
3. Confirm `src->dev->walk` + `src->dev->open` are both non-NULL.
4. Copy-in name + validate component shape (no `/`, no `\0`, no `.`, no `..`).
5. `spoor_clone(src)` → new Spoor `nc`.
6. `src->dev->walk(src, nc, &name, 1)`:
   - On failure: detach `nc->aux` (it's still the shallow copy of src's aux from `spoor_clone` — dev9p_walk replaces aux only on success; closing nc would clunk src's fid through the shared pointer). `spoor_unref(nc)`. Return -1.
   - On success: dev9p_walk has replaced `nc->aux` with a fresh `dev9p_priv` carrying the walk-allocated fid (`fid_owned=true`). `walkqid_free` the carrier.
7. `nc->dev->open(nc, omode)`:
   - On failure: `spoor_clunk(nc)` (runs `dev9p_close` → `p9_client_clunk` on the walk fid + frees the priv).
8. `handle_alloc(p, KOBJ_SPOOR, R|W|TRANSFER, nc)` → fd. On full-table: `spoor_clunk(nc)`.

The rights envelope matches `SYS_ATTACH_9P` (R|W|TRANSFER). The server enforces actual omode semantics — writing through an `OREAD`-only fid returns an Rlerror at IO time, not a rights gate failure at syscall time.

### libt surface (`usr/lib/libt/include/thyla/syscall.h`)

```
long t_walk_open(long spoor_fd, const char *name, size_t name_len, unsigned long omode);
```

With omode constants `T_OREAD`, `T_OWRITE`, `T_ORDWR`, `T_OEXEC`, `T_OTRUNC` and the `T_WALK_OPEN_NAME_MAX = 64u` cap.

### Userspace probe (`usr/stub-walk-probe/stub-walk-probe.c`)

A `~85`-line probe that drives the production kernel path:

1. `t_attach_9p(0, 1, "/", 1, 0)` → attach_fd.
2. `t_walk_open(attach_fd, "hello", 5, T_OREAD)` → hello_fd.
3. `t_read(hello_fd, buf, 64)` — must return 25, content must match `"hello from stratumd-stub\n"`.
4. Second `t_read` at offset 25 → EOF (count 0).
5. `t_close(hello_fd)` + `t_close(attach_fd)`; `t_exits(0)`.

Failure at any step prints `stub-walk-probe: <op> FAIL\n` and exits 1.

### Joey-inline (production boot path)

`do_stratumd_stub_bringup` in `usr/joey/joey.c` now does a `t_walk_open` + `t_read` + content-check between the `t_unmount(99)` and the `t_close(attach_fd)` calls. So every boot of Thylacine exercises `SYS_WALK_OPEN` end-to-end: a regression in the syscall — kernel or wire — surfaces as a joey boot failure, not just a test failure.

The joey success diagnostic widens to `pipe + spawn + attach + mount + unmount + walk_open + read`.

### Kernel test wiring

`kernel/test/test_stratumd_stub.c::test_stratumd_stub_walk_round_trip` is structurally identical to the `fs_round_trip` test, but the client is `/stub-walk-probe`:

1. Load `/stratumd-stub` + `/stub-walk-probe` via `devramfs_lookup`.
2. `pipe_create × 2` → c2s + s2c rings.
3. `rfork × 2` with `stub_exec_thunk` installing fds 0+1 in each child.
4. `wait_pid × 2`; assert both `status==0`.

Coexists with `fs_round_trip` — they cover orthogonal slices of the same stub: raw-wire mirror (fs_round_trip) vs kernel 9P client + Dev vtable + new syscall (walk_round_trip).

### Test count

The new `userspace.stratumd_stub_walk_round_trip` brings total tests 526 → 527 PASS × default + UBSan.

### Audit-trigger surfaces touched

`SYS_WALK_OPEN` is a new syscall entry point (capability checks; per CLAUDE.md trigger list) and routes through the 9P client (via dev9p_walk + dev9p_open). The handle table is touched at the install. Territory + VMO + scheduler + poll + notes + KASLR + ELF + mprotect + boot are NOT touched. A focused audit round is appropriate before promoting the syscall to non-stub callers.

---

## P5-stratumd-stub-bringup-e2 — territory-root pivot via `SYS_CHROOT`

Sub-chunk **e2** lands the v1.0 chroot mechanism per `CORVUS-DESIGN.md §10.1` (`"pivot_root (or chroot at v1.0; full pivot at v1.x)"`). The 3-option pickup-doc speculation was settled by the existing scripture in CORVUS-DESIGN: chroot at v1.0; pivot at v1.x.

The mechanism is two complementary changes:

1. **`struct Territory.root_spoor`** field + **`territory_chroot(p, source)`** kernel-internal API. Per-Territory; refcounted; cloned across rfork; clunked at Territory destruction. Spec: `specs/territory.tla::Chroot(p, s)`.
2. **`SYS_CHROOT(spoor_fd)`** syscall (= 35) + **`SYS_WALK_OPEN_FROM_ROOT == (u64)-1`** sentinel extension to `SYS_WALK_OPEN`. After `SYS_CHROOT`, a `SYS_WALK_OPEN(-1, "name", ...)` walks from the pivoted root instead of an explicit handle.

The full reference for the syscall lives in `docs/reference/77-sys-chroot.md`. The Territory-side mechanism + lifecycle + spec mapping is in `docs/reference/18-territory.md`.

### Why the pivot path runs in `/stub-walk-probe`, not joey

joey is the long-running init Proc that never exits during boot. A `t_chroot` in joey would stamp its Territory's `root_spoor` with a ref on the attach Spoor — that ref persists past joey's `t_close(attach_fd)`, holding the `p9_attached` (and its transport-Spoor refs) alive forever. stratumd-stub would never see EOF on `c2s_rd`, and joey's `t_wait_pid` would deadlock. (Confirmed empirically during e2 implementation: a chroot in joey's bringup made the test boot hang past 60 s.)

The fix: **`/stub-walk-probe` is the test vehicle for the chroot path**. The probe is a short-lived child Proc; its `t_exits(0)` triggers `territory_unref` → `spoor_clunk(root_spoor)` → `dev9p_close` on the attach Spoor → adapter teardown → transport EOF → stub sees EOF → exits cleanly. Plus the six `territory.chroot_*` kernel-internal tests cover the lifecycle in isolation.

v1.x adds `pivot_root` (or `chroot(NULL)`-clear) so a long-running Proc CAN release a chroot mid-life. Until then: chroot is one-way at v1.0; joey-as-init does not pivot.

### Stub-walk-probe extension (`usr/stub-walk-probe/stub-walk-probe.c`)

```c
// e1: walk-via-fd (preserved for regression coverage)
attach_fd = t_attach_9p(0, 1, "/", 1, 0)
hello_fd  = t_walk_open(attach_fd, "hello", 5, T_OREAD)
t_read(hello_fd, ...) → 25 bytes; assert content; t_read(...) → 0 (EOF)
t_close(hello_fd)

// e2: chroot + walk-via-root
t_chroot(attach_fd)                                          // 0
root_hello_fd = t_walk_open(T_WALK_OPEN_FROM_ROOT,           // -1 sentinel
                             "hello", 5, T_OREAD)
t_read(root_hello_fd, ...) → 25 bytes; assert content; t_read(...) → 0 (EOF)
t_close(root_hello_fd)

t_close(attach_fd)  // handle drops; territory's root_spoor still holds the Spoor
t_exits(0)          // exit releases the chroot → adapter teardown → stub EOF
```

Same kernel test `userspace.stratumd_stub_walk_round_trip` wires the extended probe.

### Joey-side change

joey does NOT exercise `t_chroot`. The existing e1 sequence (attach + mount + unmount + walk_open(attach_fd, "hello", ...)) is preserved unchanged. An explanatory `NOTE` comment in `do_stratumd_stub_bringup` documents the trap.

### Joey blob cap bump

The new chroot-related code (libt wrapper inlined elsewhere; plus the explanatory comment) is small, but it pushed joey's ELF over its 65 KiB cap (`JOEY_BLOB_MAX`). Cap raised to 128 KiB in `kernel/joey.c`, with a comment explaining the trajectory (room for several more chunks: RECOVER, USER_DELETE, ROTATE_KEY orchestration, stratumd-real swap).

### Kernel tests

6 new tests in `kernel/test/test_territory_chroot.c` (533 total — see `18-territory.md` Tests for the per-test scope):

| Test | Scope |
|---|---|
| `territory.chroot_smoke` | bump+drop refcount roundtrip |
| `territory.chroot_idempotent_same_spoor` | no-op on same-source re-chroot |
| `territory.chroot_replace_clunks_old` | MREPL-style displacement refcount |
| `territory.chroot_clone_bumps_ref` | territory_clone propagates root_spoor + bumps ref |
| `territory.chroot_destroy_drops_ref` | final-release drops root_spoor ref |
| `territory.chroot_null_returns_error` | NULL source returns -1 |

### Spec extension

`specs/territory.tla` extended with `root_spoor` state, `NONE` sentinel (string), `Chroot` / `BuggyChrootNoRefbump` actions, `ForkClone` + `BuggyDestroyLeak` updates to handle root_spoor, and `MountRefcountConsistency` formula extended to `|MountEntriesForSpoor(s)| + |{p : root_spoor[p] = s}|`. New cfg `territory_buggy_chroot_no_refbump.cfg` produces the expected counterexample (BuggyChrootNoRefbump stamps without ref bump → MountRefcountConsistency violation at depth 2 / 205 states). All 4 existing buggy cfgs still violate as expected — no regression.

### Test count

The 6 chroot tests bring total tests 527 → 533 PASS × default + UBSan, 0 UBSan runtime errors.

### Audit-trigger surfaces touched

`SYS_CHROOT` is a new syscall entry point (CLAUDE.md trigger). `kernel/territory.c` is touched (cycle-freedom, isolation, mount-refcount consistency — Territory in the trigger list). `kernel/syscall.c::sys_walk_open_handler` extended for the FROM_ROOT sentinel. Handle table touched for the source lookup. VMO + scheduler + poll + notes + KASLR + ELF + mprotect NOT touched. A focused audit round bundling SYS_WALK_OPEN (e1 deferred) + SYS_CHROOT is appropriate as the next deliverable.

---

## Composition with future chunks

- **Real stratumd swap-in (f)**: sub-chunks a–e prove the architecture with the stub. Real stratumd swap-in replaces `t_spawn_with_fds("stratumd-stub", ...)` with `t_spawn_with_fds("stratumd-system", ...)` once Phase 6's musl sysroot lets stratumd compile for Thylacine's userspace ABI.
- **`pivot_root` / `SYS_UNCHROOT` (v1.x)**: lets a long-running Proc release a chroot. With it, joey's stub-bringup could exercise chroot + revert in the same boot phase. Tracked in `CORVUS-DESIGN.md §10.1 Q2`.

---

## Status

| Item | State |
|---|---|
| `usr/stratumd-stub/stratumd-stub.c` (userspace responder) | LANDED (a) |
| `kernel/test/test_stratumd_stub.c::test_stratumd_stub_round_trip` (handshake-only demo) | LANDED (a) |
| `SYS_SPAWN_WITH_FDS` + `/stub-driver` userspace orchestrator | LANDED (b) |
| `usr/joey/joey.c::do_stratumd_stub_bringup` (inline production-boot orchestration) | LANDED (c) |
| Stub serves Twalk / Tlopen / Tread over synthetic FS (`/hello`) | LANDED (d) |
| `usr/stub-fs-probe/stub-fs-probe.c` (raw-9P client driving walk + open + read) | LANDED (d) |
| `kernel/test/test_stratumd_stub.c::test_stratumd_stub_fs_round_trip` | LANDED (d) |
| `SYS_WALK_OPEN` syscall + `t_walk_open` libt wrapper (walk-through-mount primitive) | LANDED (e1) |
| `usr/stub-walk-probe/stub-walk-probe.c` (t_attach_9p + t_walk_open + t_read probe) | LANDED (e1) |
| `kernel/test/test_stratumd_stub.c::test_stratumd_stub_walk_round_trip` | LANDED (e1) |
| Joey's `do_stratumd_stub_bringup` exercises `t_walk_open` on every boot | LANDED (e1) |
| `SYS_CHROOT` syscall + `Territory.root_spoor` + `SYS_WALK_OPEN_FROM_ROOT` sentinel | LANDED (e2) |
| `t_chroot` libt wrapper + 6 `territory.chroot_*` kernel tests + spec `Chroot` action + buggy cfg | LANDED (e2) |
| `/stub-walk-probe` exercises chroot + walk-from-root on every kernel test | LANDED (e2) |
| Joey exercises chroot (blocked by one-way v1.0 chroot semantics; see caveat) | DEFERRED to v1.x `pivot_root` |
| Real stratumd swap-in (post-Phase-6 musl sysroot) | DEFERRED (f) |

---

## Known caveats

1. **Stub exits on first EOF.** Real stratumd serves the entire mount lifetime. The stub's `for(;;) { read; respond }` loop handles arbitrary message counts, but if the client closes its tx, the stub exits — single-session. Each invocation in joey's boot path is a fresh stub Proc.
2. **Tiny synthetic FS only.** As of sub-chunk d, the stub serves exactly one root + one file (`/hello`). No subdirectories, no Twrite, no Treaddir, no Tgetattr, no extended ops. Sufficient to validate the Twalk + Tlopen + Tread wire path; expanding the synthetic tree is a small extension to the `build_response` dispatch.
3. **Walk-through-root is via the `SYS_WALK_OPEN_FROM_ROOT` sentinel (e2).** Sub-chunk e2 added the `Territory.root_spoor` pivot + the `SYS_WALK_OPEN(-1, ...)` sentinel that walks from it. The walk is still single-component at v1.0 — `t_walk_open(-1, "hello", ...)` works after `t_chroot(spoor_fd)`, but `t_walk_open(-1, "subdir/hello", ...)` does not (the production multi-component walker is a Phase 5+ separate chunk). The mount table still exists in parallel; the production walker (when it lands) will resolve through both `root_spoor` and `mounts[]`.
4. **Stub-bringup runs once per boot.** Joey runs it after corvus's exit, mounts + unmounts in a single sweep. There's no long-lived stub holding the mount across the boot's lifetime — that's the production model for real stratumd, not the stub.
5. **`target_path_id = 99` is a placeholder constant.** No string-path resolution yet for mount targets at v1.0; the abstract u32 token doesn't correspond to a visible directory in any namespace tree. Real `/sysroot` semantics land with sub-chunk e + the path-resolution syscall work in Phase 6.
6. **Fid table is per-process-lifetime, not per-session.** The stub runs once per spawn (a single client; one Tversion + Tattach + walks + reads + Tclunks + EOF). A multi-session model would need a session reset on Tversion; trivial to add when needed.
7. **No spec extension.** Pure composition over the already-spec'd 9P client + pipe + handle + mount layers. The stub-bringup pattern doesn't introduce new invariants.
