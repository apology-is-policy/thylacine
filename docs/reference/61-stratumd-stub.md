# 61. /stratumd-stub — userspace 9P responder (P5-stratumd-stub-bringup-a)

The first Phase 5 test that runs **two userspace Procs cooperating over pipes**. `/stratumd-stub` is a minimal userspace 9P responder; `/attach-probe` is the client (reused from P5-attach-probe). The kernel test framework wires them via two pipe pairs and reaps both. Until this chunk, every userspace 9P test used a kernel-thread responder. This chunk proves a real userspace process can be the 9P server — the production shape for `stratumd-system`, `stratumd-michael`, etc.

This is the first of the P5-stratumd-stub-bringup arc. The full arc is:

| Sub-chunk | Scope | Status |
|---|---|---|
| **a** (this) | Userspace responder + 2-userspace-Proc demo via kernel-supervised setup | LANDED |
| b (future) | RFFDG / SYS_SPAWN_WITH_FDS so joey can drive the orchestration from userspace | DEFERRED |
| c (future) | Long-running stratumd-stub serving multiple sequential clients | DEFERRED |
| d (future) | Pivot/chroot kernel mechanism so the stub's tree becomes Territory root | DEFERRED |
| e (future) | Full joey-in-production orchestration end-to-end | DEFERRED |

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

## Composition with future chunks

- **RFFDG / SYS_SPAWN_WITH_FDS**: lets `/joey` (userspace) drive the orchestration that the kernel test currently does. Joey opens pipes, spawns stratumd-stub with the pipe ends pre-installed, spawns a client (or runs attach-probe directly), drives the 9P traffic.
- **Long-running stratumd-stub**: today the stub exits on first EOF. For real `stratumd-system`, the responder serves the entire lifetime of the mount; multiple attach sessions, walks, opens, reads.
- **Pivot/chroot**: once a stratumd-stub-served Spoor is `SYS_MOUNT`'d, no current mechanism lets a process treat that tree as its namespace root. Pivot makes joey's `/` resolve through the mount.

---

## Status

| Item | State |
|---|---|
| `usr/stratumd-stub/stratumd-stub.c` (userspace responder) | LANDED |
| `kernel/test/test_stratumd_stub.c` (2-Proc kernel demo) | LANDED |
| Build pipeline (`usr/CMakeLists.txt`, `tools/build.sh` usr_bins, `kernel/CMakeLists.txt`) | LANDED |
| Test registry entry | LANDED |
| Joey-as-orchestrator using SYS_SPAWN (this is kernel-supervised, not joey-driven) | DEFERRED |
| Long-running stub serving multiple clients | DEFERRED |
| Pivot/chroot mechanism | DEFERRED |

---

## Known caveats

1. **Kernel-supervised setup, not joey-driven.** The kernel test framework wires the two Procs together; in production, joey would do this from userspace. RFFDG or SYS_SPAWN_WITH_FDS unblocks joey-driven orchestration in a future chunk.
2. **Stub exits on first EOF.** Real stratumd serves the entire mount lifetime. The stub's `for(;;) { read; respond }` loop handles arbitrary message counts, but if the client closes its tx, the stub exits — single-session.
3. **No walk support.** Stub responds to Tversion / Tattach / Tclunk only. Real stratumd needs Twalk, Tlopen, Tread, Twrite, etc. Each is a small extension to the dispatch in `build_response`.
4. **No spec extension.** This is pure composition over the already-spec'd 9P client + pipe + handle layers. The two-userspace-Proc pattern doesn't introduce new invariants.
