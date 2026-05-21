# 61. /stratumd-stub — userspace 9P responder (P5-stratumd-stub-bringup-a)

The first Phase 5 test that runs **two userspace Procs cooperating over pipes**. `/stratumd-stub` is a minimal userspace 9P responder; `/attach-probe` is the client (reused from P5-attach-probe). The kernel test framework wires them via two pipe pairs and reaps both. Until this chunk, every userspace 9P test used a kernel-thread responder. This chunk proves a real userspace process can be the 9P server — the production shape for `stratumd-system`, `stratumd-michael`, etc.

This is the first of the P5-stratumd-stub-bringup arc. The full arc is:

| Sub-chunk | Scope | Status |
|---|---|---|
| **a** | Userspace responder + 2-userspace-Proc demo via kernel-supervised setup | LANDED (`479d997`) |
| **b** | `SYS_SPAWN_WITH_FDS` + `/stub-driver` userspace orchestrator (production shape, separate binary) | LANDED (`73784b4`) |
| **c** | Joey runs the `/stub-driver` orchestration inline on the production boot path | LANDED (`6c1c816`) |
| **d** | Stub serves Twalk + Tlopen + Tread over a synthetic FS (`/hello`) — `/stub-fs-probe` validates | LANDED (`cde54e8`) |
| e (future) | Pivot/territory-root mechanism so the stub's tree becomes joey's `/` | DEFERRED |
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

## Composition with future chunks

- **Pivot / territory-root (e)**: with the stub now serving walkable content, the next architectural step is making the mounted Spoor the joey territory's root (or a child territory's root). Today joey's `/` resolves through devramfs; sub-chunk e adds the kernel mechanism to substitute a different Spoor as `/`. Also the natural place to land a userspace walk-through-mount primitive so joey can read `/sysroot/hello` directly without raw 9P encoding.
- **Real stratumd swap-in (f)**: sub-chunks a–e prove the architecture with the stub. Real stratumd swap-in replaces `t_spawn_with_fds("stratumd-stub", ...)` with `t_spawn_with_fds("stratumd-system", ...)` once Phase 6's musl sysroot lets stratumd compile for Thylacine's userspace ABI.

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
| Pivot/territory-root mechanism (stub's tree becomes joey's `/`) | DEFERRED (e) |
| Real stratumd swap-in (post-Phase-6 musl sysroot) | DEFERRED (f) |

---

## Known caveats

1. **Stub exits on first EOF.** Real stratumd serves the entire mount lifetime. The stub's `for(;;) { read; respond }` loop handles arbitrary message counts, but if the client closes its tx, the stub exits — single-session. Each invocation in joey's boot path is a fresh stub Proc.
2. **Tiny synthetic FS only.** As of sub-chunk d, the stub serves exactly one root + one file (`/hello`). No subdirectories, no Twrite, no Treaddir, no Tgetattr, no extended ops. Sufficient to validate the Twalk + Tlopen + Tread wire path; expanding the synthetic tree is a small extension to the `build_response` dispatch.
3. **No userspace walk-through-mount yet.** After joey mounts the stub at `target_path_id=99`, joey can't `t_read /sysroot/hello` because Thylacine has no userspace walk primitive for dev9p-backed Spoors at v1.0. The `/stub-fs-probe` validates the stub's wire surface via raw 9P over pipes (bypassing the kernel client + mount table); the joey-side read-through-mount story lands with sub-chunk e (territory-root pivot + the walk primitive that follows).
4. **Stub-bringup runs once per boot.** Joey runs it after corvus's exit, mounts + unmounts in a single sweep. There's no long-lived stub holding the mount across the boot's lifetime — that's the production model for real stratumd, not the stub.
5. **`target_path_id = 99` is a placeholder constant.** No string-path resolution yet for mount targets at v1.0; the abstract u32 token doesn't correspond to a visible directory in any namespace tree. Real `/sysroot` semantics land with sub-chunk e + the path-resolution syscall work in Phase 6.
6. **Fid table is per-process-lifetime, not per-session.** The stub runs once per spawn (a single client; one Tversion + Tattach + walks + reads + Tclunks + EOF). A multi-session model would need a session reset on Tversion; trivial to add when needed.
7. **No spec extension.** Pure composition over the already-spec'd 9P client + pipe + handle + mount layers. The stub-bringup pattern doesn't introduce new invariants.
